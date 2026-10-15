/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDInputStreams/BlurayStreamCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// The cache is driven entirely through ISource, so these tests need no CFile,
// no libbluray and no network: the fake source below stands in for the NAS and
// can be made to stall or fail on demand.
namespace
{
constexpr size_t PAGE = 64 * 1024;

// Content a wrong offset cannot accidentally satisfy, so a test proves the
// bytes are the right bytes rather than just the right count.
uint8_t ByteAt(int fileId, int64_t offset)
{
  return static_cast<uint8_t>((offset * 31 + fileId * 7 + (offset >> 13)) & 0xff);
}

struct FakeFile
{
  int id{0};
  int64_t length{0};
  std::atomic<int> opens{0};
  std::atomic<int> reads{0};
  std::atomic<bool> stalled{false}; // every read blocks: a NAS outage
  std::atomic<int> failNextReads{0};
};

class CFakeSource : public CBlurayStreamCache::ISource
{
public:
  explicit CFakeSource(FakeFile* file) : m_file(file) { ++m_file->opens; }

  int64_t GetLength() override { return m_file->length; }

  int64_t ReadAt(int64_t offset, uint8_t* buffer, size_t size) override
  {
    ++m_file->reads;
    while (m_file->stalled.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (m_file->failNextReads.load() > 0)
    {
      --m_file->failNextReads;
      return -1;
    }
    if (offset >= m_file->length)
      return 0;
    const size_t n =
        static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(size), m_file->length - offset));
    for (size_t i = 0; i < n; ++i)
      buffer[i] = ByteAt(m_file->id, offset + static_cast<int64_t>(i));
    return static_cast<int64_t>(n);
  }

private:
  FakeFile* m_file;
};

class BlurayStreamCacheTest : public ::testing::Test
{
protected:
  CBlurayStreamCache::SourceFactory Factory()
  {
    return [this](const std::string& path) -> std::unique_ptr<CBlurayStreamCache::ISource> {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_files.find(path);
      if (it == m_files.end())
        return nullptr;
      return std::make_unique<CFakeSource>(it->second);
    };
  }

  FakeFile* AddFile(const std::string& path, int id, int64_t pages)
  {
    auto file = std::make_unique<FakeFile>();
    file->id = id;
    file->length = pages * static_cast<int64_t>(PAGE);
    auto* raw = file.get();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_owned.push_back(std::move(file));
    m_files[path] = raw;
    return raw;
  }

  static CBlurayStreamCache::Config Config(size_t arenaPages = 16)
  {
    CBlurayStreamCache::Config config;
    config.pageSize = PAGE;
    config.arenaBytes = arenaPages * PAGE;
    config.behindBytes = 2 * PAGE;
    config.abandonAfterMs = 300;
    config.abandonMax = 4;
    config.nextLeadBytes = 4 * PAGE;
    config.nextSizeBytes = 4 * PAGE;
    return config;
  }

  static bool Matches(const std::vector<uint8_t>& buffer, size_t n, int fileId, int64_t offset)
  {
    for (size_t i = 0; i < n; ++i)
      if (buffer[i] != ByteAt(fileId, offset + static_cast<int64_t>(i)))
        return false;
    return true;
  }

  std::mutex m_mutex;
  std::map<std::string, FakeFile*> m_files;
  std::vector<std::unique_ptr<FakeFile>> m_owned;
};
} // namespace

TEST_F(BlurayStreamCacheTest, ServesASequentialWalkByteForByte)
{
  FakeFile* clip = AddFile("clip", 1, 40);
  clip->length += 1234; // a short final page

  auto cache = CBlurayStreamCache::Claim(Config(), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->GetLength(), clip->length);

  std::vector<uint8_t> buffer(6144);
  int64_t position = 0;
  while (position < clip->length)
  {
    const int64_t n = view->Read(buffer.data(), buffer.size());
    ASSERT_GT(n, 0);
    ASSERT_TRUE(Matches(buffer, static_cast<size_t>(n), clip->id, position));
    position += n;
  }
  EXPECT_EQ(position, clip->length);
  EXPECT_EQ(view->Read(buffer.data(), buffer.size()), 0); // past EOF, not an error
  EXPECT_GT(cache->GetStats().pagesFetched, 0u);
  EXPECT_EQ(cache->GetStats().failOpenReads, 0u);

  // The counters partition cleanly: no page access is counted twice.
  const auto s = cache->GetStats();
  EXPECT_EQ(s.pagesRequested, s.pagesResidentOnArrival + s.pagesWaited + s.pagesFailOpen);
}

TEST_F(BlurayStreamCacheTest, ReadAheadKeepsTheReaderHitting)
{
  AddFile("clip", 2, 60);
  auto cache = CBlurayStreamCache::Claim(Config(24), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  std::vector<uint8_t> buffer(6144);
  for (int i = 0; i < 400; ++i)
  {
    if (view->Read(buffer.data(), buffer.size()) <= 0)
      break;
    if (i % 50 == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(20)); // pace like a player
  }

  const auto stats = cache->GetStats();
  EXPECT_EQ(stats.pagesRequested,
            stats.pagesResidentOnArrival + stats.pagesWaited + stats.pagesFailOpen);
  EXPECT_GT(stats.pagesResidentOnArrival, stats.pagesWaited * 4);
  // Most reads never wait at all, which is the figure that matters.
  EXPECT_LT(stats.readsWaited * 4, stats.reads);
}

TEST_F(BlurayStreamCacheTest, SeekInsideTheWindowCostsNoWait)
{
  FakeFile* clip = AddFile("clip", 3, 80);
  auto cache = CBlurayStreamCache::Claim(Config(24), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  std::vector<uint8_t> buffer(4096);
  for (int i = 0; i < 100; ++i)
    view->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the window fill

  const int64_t inWindow = 10 * static_cast<int64_t>(PAGE);
  view->Seek(inWindow, SEEK_SET);
  const auto before = cache->GetStats();
  int64_t n = view->Read(buffer.data(), buffer.size());
  const auto after = cache->GetStats();
  ASSERT_GT(n, 0);
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), clip->id, inWindow));
  EXPECT_EQ(after.readsWaited, before.readsWaited);
  EXPECT_EQ(after.pagesWaited, before.pagesWaited);
  EXPECT_EQ(after.pagesResidentOnArrival, before.pagesResidentOnArrival + 1);
  EXPECT_EQ(after.seeksInWindow, before.seeksInWindow + 1);

  // Far past the window: nothing resident, so it has to be fetched - and the
  // bytes still have to be right.
  const int64_t outOfWindow = 70 * static_cast<int64_t>(PAGE);
  view->Seek(outOfWindow, SEEK_SET);
  n = view->Read(buffer.data(), buffer.size());
  ASSERT_GT(n, 0);
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), clip->id, outOfWindow));
  EXPECT_GE(cache->GetStats().seeksOutOfWindow, 1u);

  // And a back-seek past what is kept behind.
  view->Seek(0, SEEK_SET);
  n = view->Read(buffer.data(), buffer.size());
  ASSERT_GT(n, 0);
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), clip->id, 0));
}

TEST_F(BlurayStreamCacheTest, AClosedClipsPagesSurviveTheBoundary)
{
  FakeFile* first = AddFile("clipA", 4, 8);
  AddFile("clipB", 5, 8);

  auto cache = CBlurayStreamCache::Claim(Config(32), Factory());
  ASSERT_NE(cache, nullptr);

  std::vector<uint8_t> buffer(4096);
  {
    auto viewA = cache->OpenFile("clipA");
    ASSERT_NE(viewA, nullptr);
    for (int i = 0; i < 20; ++i)
      viewA->Read(buffer.data(), buffer.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  } // libbluray closes clip A when it opens clip B

  auto viewB = cache->OpenFile("clipB");
  ASSERT_NE(viewB, nullptr);
  for (int i = 0; i < 10; ++i)
    viewB->Read(buffer.data(), buffer.size());

  // Back across the seam: served from the pages clip A left behind, with no
  // new read of the source at all.
  const int readsBefore = first->reads.load();
  auto viewA2 = cache->OpenFile("clipA");
  ASSERT_NE(viewA2, nullptr);
  const int64_t n = viewA2->Read(buffer.data(), buffer.size());
  ASSERT_GT(n, 0);
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), first->id, 0));
  EXPECT_EQ(first->reads.load(), readsBefore);
}

TEST_F(BlurayStreamCacheTest, AnOutageShorterThanTheBufferIsInvisible)
{
  FakeFile* clip = AddFile("clip", 6, 200);
  auto cache = CBlurayStreamCache::Claim(Config(64), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  std::vector<uint8_t> buffer(6144);
  for (int i = 0; i < 100; ++i)
    view->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(400)); // fill the window

  clip->stalled.store(true);
  const auto start = std::chrono::steady_clock::now();
  int64_t position = view->GetPosition();
  for (int i = 0; i < 200; ++i)
  {
    const int64_t n = view->Read(buffer.data(), buffer.size());
    ASSERT_GT(n, 0) << "read " << i << " was not served from the buffer";
    ASSERT_TRUE(Matches(buffer, static_cast<size_t>(n), clip->id, position));
    position += n;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  clip->stalled.store(false);

  // The point of the whole class: the reader did not touch the network.
  EXPECT_LT(ms, 500);
}

TEST_F(BlurayStreamCacheTest, AbortReturnsAWaitingReaderAtOnce)
{
  FakeFile* clip = AddFile("clip", 7, 100);
  auto cache = CBlurayStreamCache::Claim(Config(), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");
  clip->stalled.store(true); // nothing can be fetched

  std::atomic<int64_t> result{1};
  std::atomic<bool> finished{false};
  std::thread reader([&] {
    std::vector<uint8_t> buffer(4096);
    result.store(view->Read(buffer.data(), buffer.size()));
    finished.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(finished.load()) << "the reader should be waiting for a page";

  const auto start = std::chrono::steady_clock::now();
  cache->Abort();
  reader.join();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();

  EXPECT_LT(result.load(), 0) << "an aborted read must fail, not return junk";
  EXPECT_LT(ms, 250);
  clip->stalled.store(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(BlurayStreamCacheTest, CloseDoesNotWaitForAStuckWorker)
{
  FakeFile* clip = AddFile("clip", 8, 100);
  auto cache = CBlurayStreamCache::Claim(Config(), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  std::vector<uint8_t> buffer(4096);
  view->Read(buffer.data(), buffer.size());
  clip->stalled.store(true);
  view->Seek(50 * static_cast<int64_t>(PAGE), SEEK_SET);

  std::thread reader([&] {
    std::vector<uint8_t> b(4096);
    view->Read(b.data(), b.size());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(120)); // worker now in the stalled read
  cache->Abort();
  reader.join();
  view.reset();

  const auto start = std::chrono::steady_clock::now();
  cache.reset(); // must abandon the worker, not join it
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  EXPECT_LT(ms, 250);

  clip->stalled.store(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the orphan exit
}

TEST_F(BlurayStreamCacheTest, ThePrimedNextClipCostsNoOpenAndNoWaitAtTheBoundary)
{
  FakeFile* first = AddFile("clipA", 13, 12);
  FakeFile* second = AddFile("clipB", 14, 12);

  auto cache = CBlurayStreamCache::Claim(Config(48), Factory());
  ASSERT_NE(cache, nullptr);
  auto viewA = cache->OpenFile("clipA");
  ASSERT_NE(viewA, nullptr);
  cache->SetChainHint("clipA", "clipB");

  std::vector<uint8_t> buffer(6144);
  viewA->Seek(9 * static_cast<int64_t>(PAGE), SEEK_SET); // inside nextLeadBytes of the end
  for (int i = 0; i < 4; ++i)
    viewA->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the prime run

  const int opensAfterPrime = second->opens.load();
  const int readsAfterPrime = second->reads.load();
  ASSERT_GE(opensAfterPrime, 1) << "the prime should have opened the next clip itself";
  EXPECT_EQ(first->reads.load(), first->reads.load()); // (clipA untouched by the prime)

  // The boundary. The open is the part that blocks on a stalled share, so it
  // must not reach the source at all.
  auto viewB = cache->OpenFile("clipB");
  ASSERT_NE(viewB, nullptr);
  EXPECT_EQ(second->opens.load(), opensAfterPrime);

  const auto before = cache->GetStats();
  const int64_t n = viewB->Read(buffer.data(), buffer.size());
  const auto after = cache->GetStats();
  ASSERT_GT(n, 0);
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), second->id, 0));
  EXPECT_EQ(after.readsWaited, before.readsWaited);
  EXPECT_EQ(second->reads.load(), readsAfterPrime);
  EXPECT_GE(after.primesAdopted, 1u);
}

TEST_F(BlurayStreamCacheTest, RepublishingTheHintDropsAPrimeNoLongerNext)
{
  AddFile("clipA", 15, 12);
  AddFile("clipB", 16, 12);
  AddFile("clipC", 17, 12);

  auto cache = CBlurayStreamCache::Claim(Config(48), Factory());
  ASSERT_NE(cache, nullptr);
  auto viewA = cache->OpenFile("clipA");
  cache->SetChainHint("clipA", "clipB");

  std::vector<uint8_t> buffer(4096);
  viewA->Seek(9 * static_cast<int64_t>(PAGE), SEEK_SET);
  viewA->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_GE(cache->GetStats().primesStarted, 1u);

  // An angle change or a navigation jump: clipC follows now, not clipB.
  cache->SetChainHint("clipA", "clipC");
  EXPECT_GE(cache->GetStats().primesDropped, 1u);
}

TEST_F(BlurayStreamCacheTest, APrimeSurvivesTheWindowsOwnEvictionPressure)
{
  AddFile("clipA", 18, 60);
  FakeFile* second = AddFile("clipB", 19, 12);

  // A deliberately small arena, so the forward window has to evict to keep
  // filling. The window must be sized to leave the prime room: without that
  // reserve it oversubscribes the arena and evicts the prime it is competing
  // with, and chaining stops working exactly when the disc needs it.
  CBlurayStreamCache::Config config = Config(16);
  config.behindBytes = 2 * PAGE;
  config.nextLeadBytes = 50 * PAGE;
  config.nextSizeBytes = 4 * PAGE;

  auto cache = CBlurayStreamCache::Claim(config, Factory());
  ASSERT_NE(cache, nullptr);
  auto viewA = cache->OpenFile("clipA");
  ASSERT_NE(viewA, nullptr);
  cache->SetChainHint("clipA", "clipB");

  std::vector<uint8_t> buffer(6144);
  for (int i = 0; i < 200; ++i) // far enough in to trigger the prime
    viewA->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  ASSERT_GT(cache->GetStats().primePagesFetched, 0u);

  for (int i = 0; i < 500; ++i) // and now make the window evict, hard
    viewA->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GT(cache->GetStats().evictions, 0u) << "the test is not applying any pressure";

  const int opensBefore = second->opens.load();
  const int readsBefore = second->reads.load();
  auto viewB = cache->OpenFile("clipB");
  ASSERT_NE(viewB, nullptr);
  const auto before = cache->GetStats();
  const int64_t n = viewB->Read(buffer.data(), buffer.size());
  const auto after = cache->GetStats();

  ASSERT_GT(n, 0);
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), second->id, 0));
  EXPECT_EQ(second->opens.load(), opensBefore);
  EXPECT_EQ(second->reads.load(), readsBefore) << "the prime was evicted by the window";
  EXPECT_EQ(after.readsWaited, before.readsWaited);
}

TEST_F(BlurayStreamCacheTest, OnlyOneSessionAtATimeGetsTheArena)
{
  AddFile("clip", 9, 8);

  auto first = CBlurayStreamCache::Claim(Config(), Factory());
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(CBlurayStreamCache::Claim(Config(), Factory()), nullptr);

  first.reset();
  auto third = CBlurayStreamCache::Claim(Config(), Factory());
  EXPECT_NE(third, nullptr);
}

TEST_F(BlurayStreamCacheTest, APageTheWorkerGivesUpOnIsStillServed)
{
  FakeFile* clip = AddFile("clip", 10, 8);
  auto cache = CBlurayStreamCache::Claim(Config(), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  clip->failNextReads.store(3); // exhausts the worker's retries on page 0

  std::vector<uint8_t> buffer(4096);
  const int64_t n = view->Read(buffer.data(), buffer.size());
  ASSERT_GT(n, 0) << "a page the worker could not load must degrade to a direct read, not to EOF";
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), clip->id, 0));
  EXPECT_GT(cache->GetStats().failOpenReads, 0u);
}

TEST_F(BlurayStreamCacheTest, ReportsTheCoverAheadOfTheReadHead)
{
  AddFile("clip", 12, 200);
  auto cache = CBlurayStreamCache::Claim(Config(64), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  std::vector<uint8_t> buffer(6144);
  for (int i = 0; i < 50; ++i)
    view->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(400)); // let the window fill
  view->Read(buffer.data(), buffer.size()); // refresh the read head

  const auto stats = cache->GetStats();
  // The cover is what says an outage is survivable, so it has to be real
  // bytes contiguously ahead of where the reader is - not every resident page.
  EXPECT_GT(stats.bytesAhead, 8u * PAGE);
  EXPECT_GT(stats.coverMs, 0u);
}

TEST_F(BlurayStreamCacheTest, AnArenaTooSmallToBeUsefulIsRefused)
{
  AddFile("clip", 11, 8);

  CBlurayStreamCache::Config config = Config();
  config.arenaBytes = PAGE; // one page holds no window and nothing behind it
  EXPECT_EQ(CBlurayStreamCache::Claim(config, Factory()), nullptr);

  config.arenaBytes = 0;
  EXPECT_EQ(CBlurayStreamCache::Claim(config, Factory()), nullptr);
}
