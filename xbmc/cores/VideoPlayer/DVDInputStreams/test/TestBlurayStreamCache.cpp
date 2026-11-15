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
  std::atomic<int> delayMs{0}; // every read takes this long: a slow link
  // Pages of this file the source has been asked for, in order.
  std::mutex readLog;
  std::vector<int64_t> pagesRead;
  // One lock across every source, held for the whole of an open and a read,
  // as libsmbclient's process-wide `smb` lock (and gNfsConnection) is: a read
  // stuck in the network blocks every other open and read in the process.
  std::shared_ptr<std::mutex> processLock;
};

class CFakeSource : public CBlurayStreamCache::ISource
{
public:
  explicit CFakeSource(std::shared_ptr<FakeFile> file) : m_file(std::move(file))
  {
    std::unique_lock<std::mutex> lock;
    if (m_file->processLock)
      lock = std::unique_lock<std::mutex>(*m_file->processLock);
    ++m_file->opens;
  }

  int64_t GetLength() override { return m_file->length; }

  int64_t ReadAt(int64_t offset, uint8_t* buffer, size_t size) override
  {
    std::unique_lock<std::mutex> lock;
    if (m_file->processLock)
      lock = std::unique_lock<std::mutex>(*m_file->processLock);
    ++m_file->reads;
    {
      std::lock_guard<std::mutex> logLock(m_file->readLog);
      m_file->pagesRead.push_back(offset / static_cast<int64_t>(PAGE));
    }
    while (m_file->stalled.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (m_file->delayMs.load() > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(m_file->delayMs.load()));
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
  // Shared, not borrowed: a worker abandoned in a stalled read can outlive
  // the test that started it, and must still find its file when it wakes.
  std::shared_ptr<FakeFile> m_file;
};

int64_t MsSince(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

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
    auto file = std::make_shared<FakeFile>();
    file->id = id;
    file->length = pages * static_cast<int64_t>(PAGE);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_files[path] = file;
    return file.get();
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
  std::map<std::string, std::shared_ptr<FakeFile>> m_files;
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
  // Deliberately NOT "no new source read": adopting a prime arms the forward
  // window at once, so the worker legitimately starts fetching ahead in this
  // clip. What must not happen is a source OPEN on the reader's thread - that
  // is the part that blocks on a stalled share.
  EXPECT_EQ(second->opens.load(), opensAfterPrime);
  EXPECT_GE(second->reads.load(), readsAfterPrime);
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
  EXPECT_EQ(after.readsWaited, before.readsWaited)
      << "the prime was evicted by the window it was competing with";
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

// Review finding 1. On smb:// and nfs:// every open and read in the process is
// serialised by one lock that a read holds for as long as the network does.
// A replacement worker started for a stuck one blocks OPENING its source on
// that lock, outside any read - and close must abandon it too, not join it.
TEST_F(BlurayStreamCacheTest, CloseDoesNotWaitForAWorkerBlockedOpeningBehindAStuckRead)
{
  FakeFile* clip = AddFile("clip", 20, 100);
  clip->processLock = std::make_shared<std::mutex>();

  auto cache = CBlurayStreamCache::Claim(Config(), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");
  ASSERT_NE(view, nullptr);

  clip->stalled.store(true);
  std::thread reader([&] {
    std::vector<uint8_t> b(4096);
    view->Read(b.data(), b.size());
  });
  // Past abandonAfterMs (300) plus a reader wake-up (250): the first worker
  // is abandoned and its replacement is now blocked opening the clip.
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  ASSERT_GE(cache->GetStats().workersAbandoned, 1u) << "the stuck worker was never replaced";

  cache->Abort();
  reader.join();
  view.reset();

  // The network comes back on its own after 2 s, so a close that waits for
  // it fails the test instead of hanging it.
  std::thread link([clip] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    clip->stalled.store(false);
  });
  const auto start = std::chrono::steady_clock::now();
  cache.reset();
  const int64_t ms = MsSince(start);
  clip->stalled.store(false);
  link.join();
  EXPECT_LT(ms, 250) << "close waited for a worker blocked behind the stuck read";
  std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let the orphans exit
}

// Review finding 3. A worker abandoned at close whose stalled read then FAILS
// - the normal end of a network timeout - must not touch the destroyed cache
// on its way to the retry. Caught by ASan/TSan as a use-after-free.
TEST_F(BlurayStreamCacheTest, AnAbandonedWorkerWhoseReadFailsTouchesNothing)
{
  FakeFile* clip = AddFile("clip", 21, 100);
  auto cache = CBlurayStreamCache::Claim(Config(), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  clip->stalled.store(true);
  std::thread reader([&] {
    std::vector<uint8_t> b(4096);
    view->Read(b.data(), b.size());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(120)); // worker in the stalled read
  cache->Abort();
  reader.join();
  view.reset();
  cache.reset(); // abandons the worker

  clip->failNextReads.store(1);
  clip->stalled.store(false); // the read returns, and it is an error
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// Review finding 5. abandonMax bounds threads still RUNNING. An abandoned
// thread whose read has returned has gone, and must stop counting, or a long
// film's fifth stall is never ridden through and its Stop joins again.
TEST_F(BlurayStreamCacheTest, AnAbandonedThreadThatExitedFreesItsSlot)
{
  FakeFile* clip = AddFile("clip", 22, 400);
  CBlurayStreamCache::Config config = Config();
  config.abandonMax = 1;
  auto cache = CBlurayStreamCache::Claim(config, Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  std::vector<uint8_t> buffer(4096);
  for (int stall = 0; stall < 3; ++stall)
  {
    clip->stalled.store(true);
    view->Seek((50 + 100 * stall) * static_cast<int64_t>(PAGE), SEEK_SET);
    std::thread reader([&] {
      std::vector<uint8_t> b(4096);
      view->Read(b.data(), b.size());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    clip->stalled.store(false); // the link comes back; the abandoned read returns
    reader.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(cache->GetStats().workersAbandoned, static_cast<uint64_t>(stall + 1))
        << "stall " << stall << " was not ridden through";
  }
}

// Review finding 4. A seek must not queue the page the reader is waiting for
// behind the window it just left: on a resume or chapter skip that window is
// up to the whole arena of stale fetches.
TEST_F(BlurayStreamCacheTest, ASeekIsNotQueuedBehindTheWindowItLeft)
{
  FakeFile* clip = AddFile("clip", 23, 400);
  auto cache = CBlurayStreamCache::Claim(Config(64), Factory()); // a 57-page window
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("clip");

  std::vector<uint8_t> buffer(4096);
  view->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  clip->delayMs.store(20); // a slow link: 57 stale pages would be > 1 s
  view->Read(buffer.data(), buffer.size()); // sequential: arms the window

  const int64_t target = 300 * static_cast<int64_t>(PAGE);
  size_t readsBeforeSeek = 0;
  {
    std::lock_guard<std::mutex> lock(clip->readLog);
    readsBeforeSeek = clip->pagesRead.size();
  }
  view->Seek(target, SEEK_SET);
  const auto start = std::chrono::steady_clock::now();
  const int64_t n = view->Read(buffer.data(), buffer.size());
  const int64_t ms = MsSince(start);
  ASSERT_GT(n, 0);
  EXPECT_TRUE(Matches(buffer, static_cast<size_t>(n), clip->id, target));
  EXPECT_LT(ms, 200) << "the page after the seek waited behind the old window";

  // And the old window is dropped, not fetched anyway: at most the read that
  // was already in flight at the seek.
  view->Read(buffer.data(), buffer.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  int stale = 0;
  std::string order;
  {
    std::lock_guard<std::mutex> lock(clip->readLog);
    for (size_t i = readsBeforeSeek; i < clip->pagesRead.size(); ++i)
    {
      order += std::to_string(clip->pagesRead[i]) + " ";
      if (clip->pagesRead[i] < 300)
        ++stale;
    }
  }
  EXPECT_LE(stale, 2) << "the window the seek left was fetched anyway: " << order;
  clip->delayMs.store(0);
}

// Review finding 8. A prime that yields to window work resumes with what is
// LEFT of its budget, not a fresh one, and a hint republish drops it even
// after it has yielded and re-queued itself.
TEST_F(BlurayStreamCacheTest, APrimeThatYieldsKeepsItsBudget)
{
  AddFile("clipA", 24, 200);
  FakeFile* second = AddFile("clipB", 25, 100);
  second->delayMs.store(15); // each prime page takes long enough for window work to arrive

  CBlurayStreamCache::Config config = Config(64);
  config.nextLeadBytes = 190 * PAGE; // the prime starts at once
  config.nextSizeBytes = 4 * PAGE;
  auto cache = CBlurayStreamCache::Claim(config, Factory());
  ASSERT_NE(cache, nullptr);
  auto viewA = cache->OpenFile("clipA");
  cache->SetChainHint("clipA", "clipB");

  // A steady reader: every page it consumes queues one more window page, so
  // the prime yields at almost every page boundary.
  std::vector<uint8_t> buffer(16 * 1024);
  for (int i = 0; i < 600; ++i)
  {
    if (viewA->Read(buffer.data(), buffer.size()) <= 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_EQ(cache->GetStats().primesStarted, 1u);
  EXPECT_LE(cache->GetStats().primePagesFetched, 4u) << "the prime ran past its budget";
}

TEST_F(BlurayStreamCacheTest, ARepublishDropsAPrimeThatHasYielded)
{
  AddFile("clipA", 26, 40);
  FakeFile* second = AddFile("clipB", 27, 40);
  AddFile("clipC", 28, 40);
  second->delayMs.store(40);

  CBlurayStreamCache::Config config = Config(64);
  config.nextLeadBytes = 30 * PAGE;
  config.nextSizeBytes = 20 * PAGE;
  auto cache = CBlurayStreamCache::Claim(config, Factory());
  ASSERT_NE(cache, nullptr);
  auto viewA = cache->OpenFile("clipA");
  cache->SetChainHint("clipA", "clipB");

  std::vector<uint8_t> buffer(16 * 1024);
  for (int i = 0; i < 40; ++i) // into the lead: the prime starts, and yields to the window
  {
    viewA->Read(buffer.data(), buffer.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  ASSERT_GE(cache->GetStats().primesStarted, 1u);

  cache->SetChainHint("clipA", "clipC"); // clipB is no longer next
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the read in flight lands
  const int readsAtDrop = second->reads.load();
  for (int i = 0; i < 40; ++i) // more window work, more chances to resume
  {
    viewA->Read(buffer.data(), buffer.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  EXPECT_LE(second->reads.load(), readsAtDrop) << "a dropped prime kept fetching";
}

// A disc image read whole (stream mode): libbluray reads its metadata - UDF,
// index, playlists, clip info - from the same file it streams from, so the
// stream is interrupted by small reads far away and then resumes. A jump
// drops the queued window; coming back must re-arm it, and what was already
// fetched for the stream must still be there.
TEST_F(BlurayStreamCacheTest, AnImageStreamSurvivesMetadataReadsElsewhere)
{
  FakeFile* image = AddFile("disc.iso", 29, 600);
  auto cache = CBlurayStreamCache::Claim(Config(64), Factory());
  ASSERT_NE(cache, nullptr);
  auto view = cache->OpenFile("disc.iso");
  ASSERT_NE(view, nullptr);
  image->delayMs.store(2);

  constexpr int64_t BLOCK = 2048;
  std::vector<uint8_t> buffer(3 * BLOCK * 3); // libbluray's 6144-byte aligned units, x3
  int64_t position = 100 * static_cast<int64_t>(PAGE); // the m2ts extent
  const auto streamFor = [&](int reads) {
    for (int i = 0; i < reads; ++i)
    {
      view->Seek(position, SEEK_SET);
      const int64_t n = view->Read(buffer.data(), buffer.size());
      ASSERT_GT(n, 0);
      ASSERT_TRUE(Matches(buffer, static_cast<size_t>(n), image->id, position));
      position += n;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };

  // Two reads arm the window: about 57 pages queued, few fetched yet, when
  // the first metadata read comes - the case where a jump drops real work.
  streamFor(2);
  const auto before = cache->GetStats();

  for (int jump = 0; jump < 5; ++jump)
  {
    // A clip info lookup near the start of the image, then back to the stream.
    const int64_t meta = (2 + jump) * static_cast<int64_t>(PAGE);
    view->Seek(meta, SEEK_SET);
    ASSERT_GT(view->Read(buffer.data(), BLOCK), 0);
    ASSERT_TRUE(Matches(buffer, BLOCK, image->id, meta));
    // ~55 pages each time: 5 of these walk far past the 57-page window that
    // was full before the first jump, so the window has to have re-armed.
    streamFor(200);
  }
  const auto after = cache->GetStats();

  // Only the metadata reads themselves, and the first stream reads after
  // each (the window re-arms on the second), may wait. A window that does not
  // re-arm over what the jump dropped waits on every one of those pages.
  EXPECT_LE(after.readsWaited - before.readsWaited, 5u * 3u)
      << "returning to the stream after a metadata read waited on the network";
}
