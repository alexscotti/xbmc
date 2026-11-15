/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDInputStreams/BlurayIsoCache.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace
{
constexpr size_t PAGE = 64 * 1024;
constexpr int BLOCKS_PER_PAGE = static_cast<int>(PAGE / CBlurayIsoCache::BLOCK_SIZE);

// Stands in for the image on the share. One lock around every read, as
// CDVDInputStreamBluray's m_readBlocksLock serialises the reader's and the
// worker's reads on the one handle.
class CFakeImage
{
public:
  explicit CFakeImage(int64_t pages) : m_length(pages * static_cast<int64_t>(PAGE)) {}

  CBlurayIsoCache::ReadCallback Callback()
  {
    return [this](int64_t offset, uint8_t* buffer, size_t size) -> int64_t {
      std::lock_guard<std::mutex> lock(m_readLock);
      {
        std::lock_guard<std::mutex> countLock(m_countMutex);
        ++m_reads[offset / static_cast<int64_t>(PAGE)];
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(m_delayMs.load()));
      const int64_t n = std::min<int64_t>(static_cast<int64_t>(size), m_length - offset);
      for (int64_t i = 0; i < n; ++i)
        buffer[i] = static_cast<uint8_t>((offset + i) * 13);
      return n;
    };
  }

  int ReadsOf(int64_t page)
  {
    std::lock_guard<std::mutex> lock(m_countMutex);
    return m_reads[page];
  }

  int64_t Length() const { return m_length; }
  std::atomic<int> m_delayMs{0};

private:
  int64_t m_length;
  std::mutex m_readLock;
  std::mutex m_countMutex;
  std::map<int64_t, int> m_reads;
};
} // namespace

// Review finding 7. A reader that reaches the page the worker is loading
// waits for that load, instead of queueing behind it on the handle's lock and
// then reading the same page from the share a second time.
TEST(BlurayIsoCacheTest, AReaderCatchingTheWorkerDoesNotReadThePageTwice)
{
  CFakeImage image(64);
  CBlurayIsoCache::Config config;
  config.pageSize = PAGE;
  config.maxBytes = 32 * PAGE;
  config.forwardPrefetchPages = 4;
  CBlurayIsoCache cache(image.Length(), image.Callback(), config);
  cache.Start();

  std::vector<uint8_t> buffer(PAGE);
  // Two reads in page 0: the second is sequential and queues pages 1..4.
  ASSERT_EQ(cache.ReadBlocks(buffer.data(), 0, 1), 1);
  image.m_delayMs.store(100);
  ASSERT_EQ(cache.ReadBlocks(buffer.data(), 1, 1), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(30)); // the worker is now loading page 1

  // The reader reaches page 1 while it is in flight.
  ASSERT_EQ(cache.ReadBlocks(buffer.data(), BLOCKS_PER_PAGE, 1), 1);
  EXPECT_EQ(buffer[0], static_cast<uint8_t>(PAGE * 13));
  EXPECT_EQ(image.ReadsOf(1), 1) << "page 1 was read from the share twice";

  image.m_delayMs.store(0);
  cache.Stop();
}
