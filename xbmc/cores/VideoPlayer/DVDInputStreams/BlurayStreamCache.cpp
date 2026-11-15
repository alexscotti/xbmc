/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BlurayStreamCache.h"

#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(TARGET_POSIX)
#include <sys/mman.h>
#endif

namespace
{
// The arena has one owner at a time (see Claim). A flag, not a pointer: the
// owner is whoever set it, and only that cache clears it.
std::atomic<bool> g_arenaClaimed{false};

// Abandoned workers still running, across every session: each one is a
// thread still inside a network call. It is decremented by the thread itself
// as it exits, so it bounds what is actually alive, not abandons ever made.
std::atomic<unsigned int> g_abandonedRunning{0};

int64_t NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Just the file name, for logs. Deliberately not URIUtils: everything in this
// file is kept free of the rest of Kodi so the class can be tested against a
// fake source with no Kodi around it.
std::string NameOf(const std::string& path)
{
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string WorkKey(const std::string& path, int64_t pageIndex)
{
  return path + ':' + std::to_string(pageIndex);
}

// How hard a worker tries one page before the reader is told to read it
// directly. A read that errors or comes back short is a transient on this
// link far more often than it is a real fault, so it is retried rather than
// reported as the end of the file.
constexpr unsigned int PAGE_READ_ATTEMPTS = 3;
constexpr unsigned int PAGE_RETRY_BASE_MS = 200;

// How often the stats are logged while a title plays. Only at the close is no
// use for telling whether read-ahead is keeping up during a two-hour film.
constexpr int64_t STATS_LOG_INTERVAL_MS = 30000;
} // namespace

std::shared_ptr<CBlurayStreamCache> CBlurayStreamCache::Claim(const Config& config,
                                                              SourceFactory factory)
{
  if (config.arenaBytes == 0 || !factory)
    return nullptr;

  bool expected = false;
  if (!g_arenaClaimed.compare_exchange_strong(expected, true))
  {
    CLog::Log(LOGINFO, "CBlurayStreamCache - the arena is already in use by another session; "
                       "this one reads directly");
    return nullptr;
  }

  // Not make_shared: the constructor is private and this is the only caller.
  std::shared_ptr<CBlurayStreamCache> cache(new CBlurayStreamCache(config, std::move(factory)));
  if (!cache->AllocateArena())
  {
    CLog::Log(LOGERROR, "CBlurayStreamCache - could not allocate a {} byte arena; reading directly",
              config.arenaBytes);
    return nullptr; // the destructor releases the claim
  }

  cache->m_startedMs = NowMs();
  cache->m_lastStatsLogMs = cache->m_startedMs;
  cache->StartWorker();
  CLog::Log(LOGINFO,
            "CBlurayStreamCache - {} byte arena, {} byte pages ({} pages, {} window, {} behind), "
            "abandon after {} ms",
            cache->m_arenaBytes, cache->m_config.pageSize, cache->m_pageCount,
            cache->m_windowPages, cache->m_config.behindBytes / cache->m_config.pageSize,
            cache->m_config.abandonAfterMs);
  return cache;
}

CBlurayStreamCache::CBlurayStreamCache(const Config& config, SourceFactory factory)
  : m_config(config), m_factory(std::move(factory))
{
  if (m_config.pageSize == 0)
    m_config.pageSize = 1024 * 1024;
}

bool CBlurayStreamCache::AllocateArena()
{
  m_pageCount = m_config.arenaBytes / m_config.pageSize;
  if (m_pageCount < 2)
  {
    // One page cannot hold a window and anything behind it, and the whole
    // point is to have both.
    CLog::Log(LOGERROR, "CBlurayStreamCache - arena of {} bytes is under two {} byte pages",
              m_config.arenaBytes, m_config.pageSize);
    return false;
  }
  m_arenaBytes = m_pageCount * m_config.pageSize;

#if defined(TARGET_POSIX)
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_POPULATE)
  // Fault it all in now, so the cost is real and visible in RSS from the
  // start and playback never pays a first-touch fault.
  flags |= MAP_POPULATE;
#endif
  void* mem = mmap(nullptr, m_arenaBytes, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (mem == MAP_FAILED)
    return false;
  m_arena = static_cast<uint8_t*>(mem);
#if !defined(MAP_POPULATE)
  std::memset(m_arena, 0, m_arenaBytes);
#endif
#else
  m_arena = new (std::nothrow) uint8_t[m_arenaBytes];
  if (!m_arena)
    return false;
  std::memset(m_arena, 0, m_arenaBytes);
#endif

  m_freePages.reserve(m_pageCount);
  for (size_t i = 0; i < m_pageCount; ++i)
    m_freePages.push_back(m_arena + i * m_config.pageSize);

  const size_t behindPages = m_config.behindBytes / m_config.pageSize;
  // The window has to leave room for what is kept behind AND for a prime in
  // flight, or it oversubscribes the arena: it would then evict to make room
  // for itself, and the prime - which no reader has touched yet - is the
  // first thing it would take, so chaining would quietly stop working at
  // exactly the moment it matters.
  m_primeReservePages =
      (m_config.nextSizeBytes + m_config.pageSize - 1) / m_config.pageSize;
  const size_t reserved = behindPages + m_primeReservePages + 1;
  m_windowPages = m_pageCount > reserved ? m_pageCount - reserved : 1;
  CLog::Log(LOGDEBUG,
            "CBlurayStreamCache - {} pages: {} window, {} behind, {} reserved for a prime",
            m_pageCount, m_windowPages, behindPages, m_primeReservePages);
  return true;
}

CBlurayStreamCache::~CBlurayStreamCache()
{
  Abort();
  LogStats("close");

  std::shared_ptr<WorkerCtx> ctx;
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_stopWorker = true;
    m_window.clear();
    m_runUsedMs.clear();
    m_primes.clear();
    m_queued.clear();
    ctx = m_workerCtx;
    m_workerCtx.reset();
  }
  m_queueChanged.notify_all();

  bool abandon = false;
  if (ctx)
  {
    // Join or abandon is decided only once the worker has either exited or
    // entered a source call. Deciding earlier - on whether it happens to be
    // inside one at this instant - joined a worker a moment before it started
    // closing its sources after the Abort, and on smb:// that close waits out
    // the network: measured on the box as the whole ~26 s Stop freeze this
    // class exists to remove. The worker reaches one or the other promptly:
    // it is cancelled, and every wait it makes outside a source call wakes on
    // the stop.
    std::unique_lock<std::mutex> lock(ctx->mutex);
    ctx->changed.wait(lock, [&] { return ctx->exited || ctx->blocked; });
    if (!ctx->exited && g_abandonedRunning.load() < m_config.abandonMax)
    {
      // It owns its own handle and buffer, and once abandoned it cannot
      // touch anything of ours again.
      MarkAbandonedLocked(*ctx);
      abandon = true;
    }
  }

  if (abandon)
  {
    m_worker.detach();
    {
      std::lock_guard<std::mutex> lock(m_statsMutex);
      ++m_stats.workersAbandoned;
    }
    CLog::Log(LOGINFO,
              "CBlurayStreamCache - worker abandoned (close), it is stuck in a source call; {} "
              "of {} still running",
              g_abandonedRunning.load(), m_config.abandonMax);
  }
  else if (m_worker.joinable())
    m_worker.join();

  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_files.clear();
    m_lru.clear();
    m_freePages.clear();
  }

  if (m_arena)
  {
#if defined(TARGET_POSIX)
    munmap(m_arena, m_arenaBytes);
#else
    delete[] m_arena;
#endif
    m_arena = nullptr;
  }

  g_arenaClaimed.store(false);
}

void CBlurayStreamCache::Abort()
{
  if (m_aborted.exchange(true))
    return;

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_window.clear();
    m_runUsedMs.clear();
    m_primes.clear();
    m_queued.clear();
    if (m_workerCtx)
      m_workerCtx->cancel.store(true);
  }
  m_queueChanged.notify_all();
  // Every reader waiting for a page returns now, so the demuxer ends and the
  // player thread exits without anyone waiting on the link.
  m_pageChanged.notify_all();
}

std::shared_ptr<CBlurayStreamCache::CView> CBlurayStreamCache::OpenFile(const std::string& path)
{
  if (m_aborted)
    return nullptr;

  int64_t length = 0;
  bool everRead = false;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_files.find(path);
    if (it != m_files.end())
    {
      length = it->second->length;
      everRead = it->second->everRead;
    }
  }

  if (length != 0 && !everRead)
  {
    // Consumed, so it is no longer outstanding. Without this the next hint
    // republish - which happens as soon as playback moves into this clip -
    // sees it still named as the queued prime, finds it is not the NEW next
    // clip, and counts a drop: every successful prime was tallied as both
    // adopted and dropped (4 started / 3 adopted / 4 dropped on Sin City).
    {
      std::lock_guard<std::mutex> hintLock(m_hintMutex);
      // A prime that libbluray OPENS is always a next-clip prime, which is
      // queued at offset 0; a same-clip prime has no open and is adopted on
      // the read instead.
      if (m_primeQueuedFor == PrimeKey(path, 0))
        m_primeQueuedFor.clear();
    }

    // No open is needed either way once the length is known, but only a file
    // no reader has touched is an adopted PRIME. A clip that was played and
    // closed also has its length, and re-opening it for a back-seek across
    // the seam is retention doing its job - counting that as an adoption made
    // the prime figures self-contradictory: a disc whose playlists are all
    // single-clip reported 1 started / 1 adopted / 1 dropped.
    {
      std::lock_guard<std::mutex> lock(m_statsMutex);
      ++m_stats.primesAdopted;
    }

    // Arm the forward window NOW instead of waiting for the anchor rule's
    // second read. The anchor rule exists so read-ahead does not run off
    // behind libbluray's index/clpi lookups, and those are never primed - a
    // primed clip is by construction the next thing to be read sequentially
    // from its start - so the protection is not needed here, and the delay
    // costs a wait at the boundary: 6 of them in a 12-minute run, measured,
    // even with 8 of 8 primes adopted.
    std::shared_ptr<FileState> armed;
    {
      std::lock_guard<std::mutex> lock(m_cacheMutex);
      armed = GetFileUnlocked(path);
      if (armed->lastReadEndPage < 0)
        armed->lastReadEndPage = 0;
      // A run at the head, so the read-ahead below is armed rather than
      // establishing the anchor.
      if (armed->runs[0].id == 0)
        armed->runs[0] = FileState::Run{m_nextRunId++, 0, -1, NowMs()};
    }
    QueueForwardWindow(armed, 0);
  }

  if (length == 0)
  {
    // One open to learn the length, then closed again. Nothing holds a handle
    // on a file between reads: the worker opens its own, and a fail-open read
    // opens one for that read.
    auto probe = m_factory(path);
    if (!probe)
      return nullptr;
    length = probe->GetLength();
    if (length <= 0)
      return nullptr;

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto file = GetFileUnlocked(path);
    file->length = length;
  }

  return std::make_shared<CView>(shared_from_this(), path, length);
}

std::shared_ptr<CBlurayStreamCache::FileState> CBlurayStreamCache::GetFileUnlocked(
    const std::string& path)
{
  auto it = m_files.find(path);
  if (it != m_files.end())
    return it->second;

  auto file = std::make_shared<FileState>();
  file->path = path;
  m_files.emplace(path, file);
  return file;
}

std::string CBlurayStreamCache::PrimeKey(const std::string& path, int64_t offset)
{
  return path + "@" + std::to_string(offset);
}

void CBlurayStreamCache::SetChainHint(const std::string& currentPath,
                                     const std::string& nextPath,
                                     int64_t nextOffset,
                                     int64_t currentEndOffset)
{
  std::string dropped;
  {
    std::lock_guard<std::mutex> lock(m_hintMutex);
    if (m_hintCurrent == currentPath && m_hintNext == nextPath &&
        m_hintNextOffset == nextOffset && m_hintCurrentEnd == currentEndOffset)
      return;

    // A prime taken for a clip that is no longer next cannot be adopted, so
    // stop holding it: its pages are already at the cold end of the LRU and
    // will go first.
    const std::string wanted =
        nextPath.empty() ? std::string() : PrimeKey(nextPath, nextOffset);
    if (!m_primeQueuedFor.empty() && m_primeQueuedFor != wanted)
    {
      dropped = m_primeQueuedFor;
      m_primeQueuedFor.clear();
      m_sameClipPrime.store(false);
    }
    m_hintCurrent = currentPath;
    m_hintNext = nextPath;
    m_hintNextOffset = nextOffset;
    m_hintCurrentEnd = currentEndOffset;
  }

  if (!dropped.empty())
  {
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_primes.erase(std::remove_if(m_primes.begin(), m_primes.end(),
                                    [&dropped](const PrimeJob& job) {
                                      return job.key == dropped;
                                    }),
                     m_primes.end());
    }
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.primesDropped;
  }

  if (!nextPath.empty() && nextPath == currentPath)
    CLog::Log(LOGDEBUG,
              "CBlurayStreamCache - chain hint: {} -> same clip, next playitem at {} MB",
              NameOf(currentPath), nextOffset / (1024 * 1024));
  else
    CLog::Log(LOGDEBUG, "CBlurayStreamCache - chain hint: {} -> {}",
              NameOf(currentPath),
              nextPath.empty() ? "(end of playlist)" : NameOf(nextPath));
}

void CBlurayStreamCache::MaybeQueuePrime(const std::shared_ptr<FileState>& file, int64_t offset)
{
  if (m_config.nextLeadBytes == 0 || m_config.nextSizeBytes == 0)
    return;
  if (file->length <= 0)
    return;

  PrimeJob job;
  bool sameClip = false;
  {
    std::lock_guard<std::mutex> lock(m_hintMutex);
    if (m_hintCurrent != file->path || m_hintNext.empty())
      return;
    // The lead is measured to the end of the CURRENT PLAYITEM, which is the
    // end of the file only when the next playitem is in another clip. A
    // same-clip seam sits mid-file, and measuring to EOF there would arm the
    // prime minutes late - or never, on a playitem that ends far from EOF.
    const int64_t boundary = (m_hintCurrentEnd > 0 && m_hintCurrentEnd <= file->length)
                                 ? m_hintCurrentEnd
                                 : file->length;
    if (offset >= boundary || (boundary - offset) > static_cast<int64_t>(m_config.nextLeadBytes))
      return;
    job.path = m_hintNext;
    job.offset = m_hintNextOffset;
    job.key = PrimeKey(job.path, job.offset);
    if (m_primeQueuedFor == job.key)
      return; // already queued or done
    m_primeQueuedFor = job.key;
    sameClip = (job.path == m_hintCurrent);
    if (sameClip)
    {
      m_sameClipPrimeOffset.store(job.offset);
      m_sameClipPrime.store(true);
    }
  }
  const std::string& next = job.path;

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_stopWorker || m_aborted)
      return;
    m_primes.push_back(job);
  }
  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.primesStarted;
    if (sameClip)
      ++m_stats.sameClipPrimesStarted;
  }
  m_queueChanged.notify_one();
  if (sameClip)
    CLog::Log(LOGDEBUG,
              "CBlurayStreamCache - priming {} at {} MB (same clip, {} MB) - seam {} MB away",
              NameOf(next), job.offset / (1024 * 1024),
              m_config.nextSizeBytes / (1024 * 1024), (job.offset - offset) / (1024 * 1024));
  else
    CLog::Log(LOGDEBUG, "CBlurayStreamCache - priming {} ({} MB) - {} MB left of {}",
              NameOf(next), m_config.nextSizeBytes / (1024 * 1024),
              (file->length - offset) / (1024 * 1024), NameOf(file->path));
}

int64_t CBlurayStreamCache::ReadAt(const std::string& path,
                                   int64_t offset,
                                   uint8_t* buffer,
                                   size_t size)
{
  if (!buffer || size == 0 || offset < 0)
    return -1;
  if (m_aborted)
    return -1;

  std::shared_ptr<FileState> file;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    file = GetFileUnlocked(path);
  }
  if (file->length <= 0 || offset >= file->length)
    return 0;

  const size_t wanted = static_cast<size_t>(
      std::min<int64_t>(static_cast<int64_t>(size), file->length - offset));

  // A same-clip prime is adopted by a read reaching it. There is no file_open
  // for one - libbluray seeks inside the clip it already has open - so the
  // open-side adoption never fires and, without this, the next hint republish
  // would find the prime still outstanding and count it DROPPED. The flag is
  // false on every ordinary read, so this costs two relaxed loads.
  if (m_sameClipPrime.load(std::memory_order_relaxed) &&
      offset >= m_sameClipPrimeOffset.load(std::memory_order_relaxed))
  {
    bool adopted = false;
    {
      std::lock_guard<std::mutex> lock(m_hintMutex);
      if (m_sameClipPrime.load() && path == m_hintNext &&
          m_primeQueuedFor == PrimeKey(m_hintNext, m_hintNextOffset))
      {
        m_primeQueuedFor.clear();
        m_sameClipPrime.store(false);
        adopted = true;
      }
    }
    if (adopted)
    {
      std::lock_guard<std::mutex> lock(m_statsMutex);
      ++m_stats.primesAdopted;
      // This branch is reached ONLY by a same-clip adoption: the open-side
      // path above handles every prime that crossed into another file.
      ++m_stats.sameClipPrimesAdopted;
    }
  }

  bool fileHadBeenRead = false;
  bool filePrimed = false;
  int64_t anchorBefore = -1;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    fileHadBeenRead = file->everRead;
    anchorBefore = file->lastReadEndPage;
    // Resident pages on a file no reader has touched can only be a prime's.
    filePrimed = !file->everRead && !file->pages.empty();
    file->everRead = true;
  }
  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.reads;
  }

  size_t done = 0;
  bool servedWholeFromCache = true;
  bool readWaited = false;
  while (done < wanted)
  {
    const int64_t pageIndex = PageOf(offset + static_cast<int64_t>(done));
    bool servedFromCache = false;
    bool readDirect = false;
    size_t copied = 0;

    {
      std::unique_lock<std::mutex> lock(m_cacheMutex);
      const int64_t waitStart = NowMs();
      bool waited = false;
      bool counted = false;

      {
        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        ++m_stats.pagesRequested;
      }

      while (true)
      {
        if (m_aborted)
          return done > 0 ? static_cast<int64_t>(done) : -1;

        if (file->pages.find(pageIndex) != file->pages.end())
        {
          bool shortPage = false;
          copied = CopyFromPageUnlocked(*file, pageIndex, offset + static_cast<int64_t>(done),
                                        buffer + done, wanted - done, shortPage);
          servedFromCache = true;
          if (!counted)
          {
            counted = true;
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            // Exactly one of these per page access, and a page we waited for
            // is NOT also counted as resident-on-arrival.
            if (waited)
              ++m_stats.pagesWaited;
            else
              ++m_stats.pagesResidentOnArrival;
          }
          if (shortPage && copied == 0)
            return done > 0 ? static_cast<int64_t>(done) : -1;
          break;
        }

        if (file->failedPages.erase(pageIndex) != 0)
        {
          if (!counted)
          {
            counted = true;
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            ++m_stats.pagesFailOpen;
          }
          // The worker gave up on this page. Read it here instead; the mark is
          // already cleared, so the next pass over it tries the cache again.
          readDirect = true;
          break;
        }

        // Not resident: make sure it is queued, then wait for it rather than
        // reading it on this thread. This is the whole ride-through: the
        // thread that must stay responsive never enters a network read.
        lock.unlock();
        QueuePage(file, pageIndex, true);
        lock.lock();
        if (file->pages.find(pageIndex) != file->pages.end() ||
            file->failedPages.find(pageIndex) != file->failedPages.end() || m_aborted)
          continue;

        waited = true;
        if (!m_pageChanged.wait_for(lock, std::chrono::milliseconds(250), [&] {
              return m_aborted || file->pages.find(pageIndex) != file->pages.end() ||
                     file->failedPages.find(pageIndex) != file->failedPages.end();
            }))
        {
          // Nothing arrived. If the worker is wedged in a read, replace it so
          // the window starts filling again the moment the link is back.
          lock.unlock();
          CheckWorkerStall();
          lock.lock();
        }
      }

      if (waited)
      {
        servedWholeFromCache = false;
        readWaited = true;
        const uint64_t ms = static_cast<uint64_t>(std::max<int64_t>(0, NowMs() - waitStart));
        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        // Attribute it. anchorBefore is this file's sequential position as it
        // was when the read started, so the classification is not confused by
        // the window update that follows.
        if (!fileHadBeenRead)
        {
          if (m_stats.reads <= 1)
            ++m_stats.waitsFirstReadOfTitle;
          else
          {
            ++m_stats.waitsFirstReadOfFile;
            if (filePrimed)
              ++m_stats.waitsFirstReadPrimed;
            else
              ++m_stats.waitsFirstReadUnprimed;
          }
        }
        else if (anchorBefore < 0 || PageOf(offset) < anchorBefore ||
                 PageOf(offset) > anchorBefore + 1)
          ++m_stats.waitsAfterSeek;
        else
          ++m_stats.waitsSequential;
        m_stats.longestStallRiddenMs = std::max(m_stats.longestStallRiddenMs, ms);
        if (ms >= 10)
          ++m_stats.waits10ms;
        if (ms >= 100)
          ++m_stats.waits100ms;
        if (ms >= 1000)
          ++m_stats.waits1000ms;
        m_stats.maxWaitMs = std::max(m_stats.maxWaitMs, ms);
      }
    }

    if (readDirect)
    {
      if (m_aborted)
        return done > 0 ? static_cast<int64_t>(done) : -1;
      servedWholeFromCache = false;
      readWaited = true;
      const int64_t n =
          ReadDirect(path, offset + static_cast<int64_t>(done), buffer + done, wanted - done);
      {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++m_stats.failOpenReads;
      }
      if (n <= 0)
        return done > 0 ? static_cast<int64_t>(done) : -1;
      done += static_cast<size_t>(n);
      continue;
    }

    if (!servedFromCache || copied == 0)
      return done > 0 ? static_cast<int64_t>(done) : -1;

    done += copied;
  }

  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.bytes += done;
    if (readWaited)
      ++m_stats.readsWaited;
  }

  // Where the cover is measured from.
  m_head.store(file.get(), std::memory_order_release);
  m_headOffset.store(offset + static_cast<int64_t>(done), std::memory_order_release);

  const int64_t firstPage = PageOf(offset);
  const int64_t lastPage = PageOf(offset + static_cast<int64_t>(done) - 1);

  // Classify the jump before QueueForwardWindow moves the anchor, or every
  // read looks sequential.
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    const int64_t previousEnd = file->lastReadEndPage;
    const bool sequential =
        previousEnd < 0 || (firstPage >= previousEnd && firstPage <= previousEnd + 1);
    if (!sequential)
    {
      const bool wasResident = servedWholeFromCache;
      std::lock_guard<std::mutex> statsLock(m_statsMutex);
      if (wasResident)
        ++m_stats.seeksInWindow;
      else
        ++m_stats.seeksOutOfWindow;
    }
  }

  QueueForwardWindow(file, lastPage);
  MaybeQueuePrime(file, offset + static_cast<int64_t>(done));
  MaybeLogStats();
  return static_cast<int64_t>(done);
}

size_t CBlurayStreamCache::CopyFromPageUnlocked(FileState& file,
                                                int64_t pageIndex,
                                                int64_t offset,
                                                uint8_t* buffer,
                                                size_t size,
                                                bool& shortPage)
{
  auto it = file.pages.find(pageIndex);
  if (it == file.pages.end())
    return 0;

  const int64_t pageStart = pageIndex * static_cast<int64_t>(m_config.pageSize);
  const int64_t pageEnd = pageStart + static_cast<int64_t>(it->second.validBytes);
  if (offset >= pageEnd)
  {
    shortPage = true;
    return 0;
  }

  const size_t chunk =
      static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(size), pageEnd - offset));
  std::memcpy(buffer, it->second.data + (offset - pageStart), chunk);
  TouchPageUnlocked(file, pageIndex);

  shortPage = (offset + static_cast<int64_t>(chunk)) >= pageEnd &&
              it->second.validBytes < m_config.pageSize;
  return chunk;
}

void CBlurayStreamCache::InsertPageUnlocked(const std::shared_ptr<FileState>& file,
                                            int64_t pageIndex,
                                            const uint8_t* data,
                                            size_t validBytes,
                                            bool prime)
{
  auto existing = file->pages.find(pageIndex);
  if (existing != file->pages.end())
  {
    std::memcpy(existing->second.data, data, validBytes);
    existing->second.validBytes = validBytes;
    TouchPageUnlocked(*file, pageIndex);
    return;
  }

  uint8_t* page = nullptr;
  if (!ReclaimPageUnlocked(page))
    return;

  std::memcpy(page, data, validBytes);
  if (prime)
  {
    m_lru.push_back({file.get(), pageIndex});
    file->pages.emplace(pageIndex, Resident{page, validBytes, std::prev(m_lru.end())});
  }
  else
  {
    m_lru.push_front({file.get(), pageIndex});
    file->pages.emplace(pageIndex, Resident{page, validBytes, m_lru.begin()});
  }
}

void CBlurayStreamCache::TouchPageUnlocked(FileState& file, int64_t pageIndex)
{
  auto it = file.pages.find(pageIndex);
  if (it == file.pages.end())
    return;
  if (it->second.lruIt != m_lru.begin())
    m_lru.splice(m_lru.begin(), m_lru, it->second.lruIt);
  it->second.lruIt = m_lru.begin();
}

bool CBlurayStreamCache::ReclaimPageUnlocked(uint8_t*& page)
{
  if (!m_freePages.empty())
  {
    page = m_freePages.back();
    m_freePages.pop_back();
    return true;
  }

  // Lowest class first, and least recently used within a class. Plain LRU is
  // not enough here: it would take the prime (never touched, so always
  // oldest) ahead of a behind page nobody will read again, and within the
  // window it would take the EARLIEST fetched page, which is the one nearest
  // the reader - the worst of the lot.
  auto victim = m_lru.end();
  auto best = PageClass::Window;
  int64_t bestDistance = -1;

  for (auto it = m_lru.begin(); it != m_lru.end(); ++it)
  {
    const FileState* file = it->first;
    if (file->pages.find(it->second) == file->pages.end())
      continue; // stale list entry

    const PageClass cls = ClassifyUnlocked(file, it->second);
    if (victim == m_lru.end() || cls < best)
    {
      victim = it;
      best = cls;
      bestDistance = it->second;
      continue;
    }
    if (cls != best)
      continue;

    // Within a class: for the window, the page FARTHEST ahead of the reader,
    // since the reader reaches it last. For the others, list order already
    // puts the least recently used at the back, so keep walking to it.
    if (cls == PageClass::Window)
    {
      if (it->second > bestDistance)
      {
        victim = it;
        bestDistance = it->second;
      }
    }
    else
    {
      victim = it;
      bestDistance = it->second;
    }
  }

  if (victim == m_lru.end())
    return false;

  FileState* file = victim->first;
  auto fileIt = file->pages.find(victim->second);
  if (fileIt == file->pages.end())
    return false;

  page = fileIt->second.data;
  file->pages.erase(fileIt);
  m_lru.erase(victim);
  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.evictions;
  }
  return true;
}

CBlurayStreamCache::PageClass CBlurayStreamCache::ClassifyUnlocked(const FileState* file,
                                                                  int64_t pageIndex) const
{
  // Runs under m_cacheMutex, so the head is read from the atomics rather
  // than behind a second lock. A slightly stale head only means a slightly
  // wrong eviction choice, which is fine - but it has to be read ATOMICALLY
  // to mean even that.
  const FileState* head = m_head.load(std::memory_order_acquire);
  if (!head || file != head)
  {
    // Another file: either a clip being primed that nobody has read yet, or
    // one that was played and closed, whose pages are only still here to
    // serve a back-seek across the seam. The latter ranks with what is
    // behind, because that is what it is.
    return file->everRead ? PageClass::Behind : PageClass::Prime;
  }

  const int64_t headPage =
      m_headOffset.load(std::memory_order_acquire) / static_cast<int64_t>(m_config.pageSize);
  if (pageIndex >= headPage)
    return PageClass::Window;
  // Behind the head - unless it is in the window of another run of this
  // file: one file read in two places (a disc image) has the head in one of
  // them at a time, and the other's window is not behind anything.
  for (const auto& r : file->runs)
    if (r.id != 0 && pageIndex >= r.lastEnd && pageIndex <= r.horizon)
      return PageClass::Window;
  return PageClass::Behind;
}

namespace
{
// A run nobody has read for this long has its queued read-ahead dropped: the
// reader has left it (a resume, a chapter skip), so fetching it is waste.
// Interleaved runs are read many times a second each, so they never get here.
constexpr int64_t RUN_IDLE_MS = 2000;
} // namespace

void CBlurayStreamCache::QueueForwardWindow(const std::shared_ptr<FileState>& file, int64_t lastPage)
{
  const int64_t maxPage = (file->length - 1) / static_cast<int64_t>(m_config.pageSize);
  const int64_t now = NowMs();
  int64_t windowEnd = 0;

  int64_t queueFrom = 0;
  uint64_t runId = 0;
  bool sequential = false;
  std::vector<uint64_t> gone;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    file->lastReadEndPage = lastPage;

    // Only read ahead for a reader walking forward. libbluray interleaves
    // index/clip lookups with the stream, and reading ahead of one of those
    // pulls a window nobody wants: a read that continues no run starts one,
    // and only its next read queues anything.
    FileState::Run* run = nullptr;
    for (auto& r : file->runs)
      if (r.id != 0 && lastPage >= r.lastEnd && lastPage <= r.lastEnd + 1)
        run = &r;

    if (!run)
    {
      // A new run, in a free slot or in place of the least recently read.
      FileState::Run* slot = &file->runs[0];
      for (auto& r : file->runs)
      {
        if (r.id == 0)
        {
          slot = &r;
          break;
        }
        if (r.usedMs < slot->usedMs)
          slot = &r;
      }
      if (slot->id != 0)
        gone.push_back(slot->id);
      *slot = FileState::Run{m_nextRunId++, lastPage, -1, now};
    }
    else
    {
      sequential = true;
      run->lastEnd = lastPage;
      run->usedMs = now;
      runId = run->id;
      // The window is shared by the runs reading ahead now, or two of them
      // oversubscribe the arena and each evicts the other's window.
      int64_t active = 0;
      for (const auto& r : file->runs)
        if (r.id != 0 && r.horizon >= 0 && now - r.usedMs <= RUN_IDLE_MS)
          ++active;
      if (run->horizon < 0)
        ++active; // this one is about to start
      const int64_t share =
          std::max<int64_t>(1, static_cast<int64_t>(m_windowPages) / std::max<int64_t>(1, active));
      windowEnd = std::min<int64_t>(maxPage, lastPage + share);
      queueFrom = std::max<int64_t>(lastPage + 1, run->horizon + 1);
      if (queueFrom <= windowEnd)
        run->horizon = windowEnd;
    }

    for (auto& r : file->runs)
      if (r.id != 0 && r.id != runId && now - r.usedMs > RUN_IDLE_MS)
      {
        gone.push_back(r.id);
        r = FileState::Run{};
      }
  }

  if (!gone.empty())
    DropRunWork(gone);
  if (!sequential)
    return;

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_runUsedMs[runId] = now;
  }
  for (int64_t page = queueFrom; page <= windowEnd; ++page)
    QueuePage(file, page, false, runId);
}

void CBlurayStreamCache::DropRunWork(const std::vector<uint64_t>& runIds)
{
  std::lock_guard<std::mutex> lock(m_queueMutex);
  m_window.erase(std::remove_if(m_window.begin(), m_window.end(),
                                [&](const Work& work) {
                                  if (work.urgent || work.runId == 0 ||
                                      std::find(runIds.begin(), runIds.end(), work.runId) ==
                                          runIds.end())
                                    return false;
                                  m_queued.erase(WorkKey(work.file->path, work.pageIndex));
                                  return true;
                                }),
                 m_window.end());
  for (const uint64_t id : runIds)
    m_runUsedMs.erase(id);
}

CBlurayStreamCache::Work CBlurayStreamCache::PopWindowLocked()
{
  auto pick = m_window.begin();
  if (!pick->urgent)
  {
    // The nearest page (queue order) of the most recently read run.
    int64_t best = -1;
    for (auto it = m_window.begin(); it != m_window.end(); ++it)
    {
      if (it->urgent)
      {
        pick = it;
        break;
      }
      const auto used = m_runUsedMs.find(it->runId);
      const int64_t usedMs = used != m_runUsedMs.end() ? used->second : 0;
      if (usedMs > best)
      {
        best = usedMs;
        pick = it;
      }
    }
  }
  Work work = *pick;
  m_window.erase(pick);
  m_queued.erase(WorkKey(work.file->path, work.pageIndex));
  return work;
}

void CBlurayStreamCache::QueuePage(const std::shared_ptr<FileState>& file,
                                   int64_t pageIndex,
                                   bool urgent,
                                   uint64_t runId)
{
  if (pageIndex < 0 || file->length <= 0)
    return;
  if (pageIndex > (file->length - 1) / static_cast<int64_t>(m_config.pageSize))
    return;

  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (file->pages.find(pageIndex) != file->pages.end())
      return;
  }

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_stopWorker || m_aborted)
      return;
    if (!m_queued.insert(WorkKey(file->path, pageIndex)).second)
    {
      if (!urgent)
        return;
      // Already queued, maybe far down: a reader is waiting for it now.
      auto it = std::find_if(m_window.begin(), m_window.end(), [&](const Work& work) {
        return work.file == file && work.pageIndex == pageIndex;
      });
      if (it == m_window.end())
        return;
      Work work = *it;
      work.urgent = true;
      m_window.erase(it);
      m_window.push_front(work);
    }
    else if (urgent)
    {
      // The page a reader is waiting for goes ahead of all read-ahead.
      m_window.push_front(Work{file, pageIndex, 0, true});
    }
    else
    {
      // Read-ahead always goes to the back: the queue is ordered by distance
      // ahead of the reader, so the nearest outstanding page stays at the
      // head, where the worker pops. Feeding the far end first starves the
      // very next read.
      m_window.push_back(Work{file, pageIndex, runId, false});
    }
  }
  m_queueChanged.notify_one();
}

std::shared_ptr<CBlurayStreamCache::WorkerCtx> CBlurayStreamCache::NewWorkerCtx()
{
  auto ctx = std::make_shared<WorkerCtx>();
  ctx->owner = this;
  ctx->factory = m_factory;
  ctx->buffer.resize(m_config.pageSize);
  return ctx;
}

void CBlurayStreamCache::StartWorker()
{
  auto ctx = NewWorkerCtx();
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_stopWorker = false;
    m_workerCtx = ctx;
  }
  m_worker = std::thread(&CBlurayStreamCache::Worker, ctx);
}

bool CBlurayStreamCache::CheckWorkerStall()
{
  std::unique_lock<std::mutex> lock(m_queueMutex);
  if (m_stopWorker || !m_workerCtx || g_abandonedRunning.load() >= m_config.abandonMax)
    return false;

  if (!AbandonIfBlockedLocked(lock, "stalled", m_config.abandonAfterMs))
    return false;
  if (m_stopWorker)
    return true;

  // A replacement, so the window fills again as soon as the link returns.
  // On smb:// and nfs:// it will block in its first open until the stuck
  // read gives up the process-wide lock; that open is abandonable too.
  auto ctx = NewWorkerCtx();
  m_workerCtx = ctx;
  m_worker = std::thread(&CBlurayStreamCache::Worker, ctx);
  return true;
}

bool CBlurayStreamCache::AbandonIfBlockedLocked(std::unique_lock<std::mutex>& queueLock,
                                                const char* reason,
                                                int64_t minBlockedMs)
{
  auto ctx = m_workerCtx;
  if (!ctx)
    return false;

  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    // The decision is taken under the same lock the worker takes on its way
    // out of a source call, so it cannot pass a worker that has just come
    // out and is about to use the cache.
    if (!ctx->blocked)
      return false;
    const int64_t since = ctx->blockedSinceMs.load();
    if (since == 0 || NowMs() - since < minBlockedMs)
      return false;
    MarkAbandonedLocked(*ctx);
  }

  m_workerCtx.reset();
  if (m_worker.joinable())
    m_worker.detach();

  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.workersAbandoned;
  }

  const unsigned int running = g_abandonedRunning.load();
  queueLock.unlock();
  CLog::Log(LOGINFO,
            "CBlurayStreamCache - worker abandoned ({}), it is stuck in a source call; {} of {} "
            "still running",
            reason, running, m_config.abandonMax);
  queueLock.lock();
  return true;
}

void CBlurayStreamCache::MarkAbandonedLocked(WorkerCtx& ctx)
{
  ctx.owner = nullptr;
  ctx.abandoned = true;
  ctx.cancel.store(true);
  // Counted here, under ctx.mutex, and uncounted by the thread under the
  // same lock as it exits, so the two can never miss each other.
  ++g_abandonedRunning;
}

void CBlurayStreamCache::Worker(std::shared_ptr<WorkerCtx> ctx)
{
  while (!ctx->cancel.load())
  {
    // Not inside a source call here, so this worker cannot be abandoned
    // until it next enters one: a non-null owner stays valid throughout.
    CBlurayStreamCache* owner = nullptr;
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      owner = ctx->owner;
    }
    if (!owner)
      break;

    if (!owner->RunOneWorkItem(*ctx))
      break;
  }

  // Sources are closed here, on this thread, as an abandonable call: on a
  // dead link the close is what takes the time, and it is nobody's wait but
  // this thread's.
  CloseSources(*ctx, nullptr);
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->exited = true;
    if (ctx->abandoned)
      --g_abandonedRunning;
  }
  ctx->changed.notify_all();
}

bool CBlurayStreamCache::BeginBlocking(WorkerCtx& ctx)
{
  std::lock_guard<std::mutex> lock(ctx.mutex);
  if (!ctx.owner || ctx.cancel.load())
    return false;
  ctx.blocked = true;
  ctx.blockedSinceMs.store(NowMs());
  ctx.changed.notify_all();
  return true;
}

bool CBlurayStreamCache::CloseSources(WorkerCtx& ctx, const std::string* path)
{
  {
    std::lock_guard<std::mutex> lock(ctx.mutex);
    ctx.blocked = true;
    ctx.blockedSinceMs.store(NowMs());
  }
  ctx.changed.notify_all();
  if (path)
    ctx.sources.erase(*path);
  else
    ctx.sources.clear();
  return EndBlocking(ctx);
}

bool CBlurayStreamCache::EndBlocking(WorkerCtx& ctx)
{
  std::lock_guard<std::mutex> lock(ctx.mutex);
  ctx.blocked = false;
  ctx.blockedSinceMs.store(0);
  return ctx.owner != nullptr;
}

CBlurayStreamCache::ISource* CBlurayStreamCache::GetWorkerSource(WorkerCtx& ctx,
                                                                 const std::string& path,
                                                                 bool& stop)
{
  stop = false;
  auto it = ctx.sources.find(path);
  if (it != ctx.sources.end())
    return it->second.get();

  // Two is enough for the current clip and the one being primed; anything
  // older is a retained file nobody is reading.
  if (ctx.sources.size() >= 2 && !CloseSources(ctx, nullptr))
  {
    stop = true;
    return nullptr;
  }

  // An open is a network call like any read: on smb:// it waits on the
  // process-wide lock behind any stuck read, so it is abandonable the same way.
  if (!BeginBlocking(ctx))
  {
    stop = true;
    return nullptr;
  }
  auto source = ctx.factory(path);
  const bool live = EndBlocking(ctx);
  ISource* raw = source.get();
  if (source)
    ctx.sources.emplace(path, std::move(source)); // ours either way; closed by this thread
  if (!live)
  {
    stop = true;
    return nullptr;
  }
  return raw;
}

bool CBlurayStreamCache::RunOneWorkItem(WorkerCtx& ctx)
{
  Work work;
  {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_queueChanged.wait(lock, [&] {
      return m_stopWorker || ctx.cancel.load() || m_workerCtx.get() != &ctx || !m_window.empty() ||
             !m_primes.empty();
    });
    if (m_stopWorker || ctx.cancel.load() || m_workerCtx.get() != &ctx)
      return false;

    // Window before prime, always: the clip being watched must not starve so
    // the next one can be got ready.
    if (m_window.empty())
    {
      const PrimeJob prime = m_primes.front();
      m_primes.pop_front();
      lock.unlock();
      RunPrime(ctx, prime);
      return true;
    }

    work = PopWindowLocked();
  }

  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (work.file->pages.find(work.pageIndex) != work.file->pages.end())
      return true;
  }

  const int64_t offset = work.pageIndex * static_cast<int64_t>(m_config.pageSize);
  const size_t bytesToRead = static_cast<size_t>(
      std::min<int64_t>(static_cast<int64_t>(m_config.pageSize), work.file->length - offset));
  if (bytesToRead == 0)
    return true;

  int64_t read = -1;
  for (unsigned int attempt = 0; attempt < PAGE_READ_ATTEMPTS; ++attempt)
  {
    bool stop = false;
    ISource* source = GetWorkerSource(ctx, work.file->path, stop);
    if (stop)
      return false; // cancelled, or abandoned in the open: touch nothing

    // A failed open is retried like a failed read. Giving up on it at once
    // marked the page failed on the first attempt, and the reader then read
    // it on its own thread - the network call this cache keeps it out of.
    if (source)
    {
      if (!BeginBlocking(ctx))
        return false;
      read = source->ReadAt(offset, ctx.buffer.data(), bytesToRead);
      if (!EndBlocking(ctx))
        return false; // abandoned while we were in the read; touch nothing

      if (read == static_cast<int64_t>(bytesToRead))
        break;

      // Short or failed. Drop the handle - on this link that is usually a
      // transient the next open rides through - and back off before retrying,
      // rather than reporting it as the end of the file.
      if (!CloseSources(ctx, &work.file->path))
        return false;
    }
    read = -1;
    {
      std::lock_guard<std::mutex> statsLock(m_statsMutex);
      ++m_stats.retries;
    }
    if (attempt + 1 < PAGE_READ_ATTEMPTS)
    {
      // In slices, so a close does not have to wait out the back-off.
      const int64_t until = NowMs() + (PAGE_RETRY_BASE_MS << attempt);
      while (!ctx.cancel.load() && NowMs() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ctx.cancel.load())
      return false;
  }

  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (read == static_cast<int64_t>(bytesToRead))
    {
      InsertPageUnlocked(work.file, work.pageIndex, ctx.buffer.data(), bytesToRead);
      std::lock_guard<std::mutex> statsLock(m_statsMutex);
      ++m_stats.pagesFetched;
    }
    else
    {
      work.file->failedPages.insert(work.pageIndex);
      std::lock_guard<std::mutex> statsLock(m_statsMutex);
      ++m_stats.pageFailures;
    }
  }
  m_pageChanged.notify_all();
  return true;
}

void CBlurayStreamCache::RunPrime(WorkerCtx& ctx, const PrimeJob& job)
{
  const std::string& path = job.path;
  bool stop = false;
  ISource* source = GetWorkerSource(ctx, path, stop);
  if (stop)
    return;
  if (!source)
  {
    CLog::Log(LOGDEBUG, "CBlurayStreamCache - prime of {} could not be opened",
              NameOf(path));
    return;
  }

  // GetLength is a source call too: on smb:// it takes the process-wide lock,
  // which a stuck read anywhere else holds, so it is abandonable like one.
  if (!BeginBlocking(ctx))
    return;
  const int64_t length = source->GetLength();
  if (!EndBlocking(ctx))
    return;
  if (length <= 0)
    return;

  std::shared_ptr<FileState> file;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    file = GetFileUnlocked(path);
    // Published before any byte is read: from here libbluray's file_open of
    // this clip needs no open of its own.
    //
    // Only when it is not known yet. A same-clip prime targets the file the
    // reader is ALREADY reading, and readers consult length unlocked, so
    // rewriting it here is a data race - TSan caught exactly that against
    // QueuePage once same-clip priming made this reachable (2026-10-17). The
    // value cannot differ: it is the same file.
    if (file->length <= 0)
      file->length = length;
  }

  // A same-clip prime starts at the next playitem's first packet, not at 0.
  const int64_t firstPage = (job.offset > 0)
                                ? job.offset / static_cast<int64_t>(m_config.pageSize)
                                : 0;
  const int64_t lastPage = (length - 1) / static_cast<int64_t>(m_config.pageSize);
  if (firstPage > lastPage)
    return;
  // A prime resumed after a yield gets what is left of its budget, not a new
  // one: re-computing it from the resume offset let every yield extend the
  // prime by another full nextSizeBytes.
  const int64_t budget =
      job.pagesLeft >= 0
          ? job.pagesLeft
          : static_cast<int64_t>((m_config.nextSizeBytes + m_config.pageSize - 1) /
                                 m_config.pageSize);
  const int64_t pages = std::min<int64_t>(budget, lastPage - firstPage + 1);

  for (int64_t n = 0; n < pages; ++n)
  {
    const int64_t page = firstPage + n;
    if (ctx.cancel.load() || m_aborted)
      return;

    // Still wanted? A republished hint drops a prime that is no longer next,
    // and an adopted one is the window's business from here. The drop in
    // SetChainHint only reaches a prime that is QUEUED; this reaches the one
    // that is running.
    {
      std::lock_guard<std::mutex> lock(m_hintMutex);
      if (m_primeQueuedFor != job.key)
        return;
    }

    // Yield at every page boundary: if the clip being watched needs a page,
    // it goes first and the rest of this prime waits its turn.
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      if (!m_window.empty())
      {
        // Resume where this prime got to, with what is left of its budget.
        m_primes.push_front(
            PrimeJob{path, page * static_cast<int64_t>(m_config.pageSize), job.key, pages - n});
        return;
      }
    }

    {
      std::lock_guard<std::mutex> lock(m_cacheMutex);
      if (file->pages.find(page) != file->pages.end())
        continue;
    }

    const int64_t offset = page * static_cast<int64_t>(m_config.pageSize);
    const size_t want = static_cast<size_t>(
        std::min<int64_t>(static_cast<int64_t>(m_config.pageSize), length - offset));

    if (!BeginBlocking(ctx))
      return;
    const int64_t read = source->ReadAt(offset, ctx.buffer.data(), want);
    if (!EndBlocking(ctx))
      return; // abandoned while we were in the read; touch nothing

    if (read != static_cast<int64_t>(want))
    {
      CloseSources(ctx, &path);
      return; // a prime is best effort; the boundary just stays cold
    }

    {
      std::lock_guard<std::mutex> lock(m_cacheMutex);
      InsertPageUnlocked(file, page, ctx.buffer.data(), want, true);
      std::lock_guard<std::mutex> statsLock(m_statsMutex);
      ++m_stats.primePagesFetched;
    }
    m_pageChanged.notify_all();
  }
}

int64_t CBlurayStreamCache::ReadDirect(const std::string& path,
                                       int64_t offset,
                                       uint8_t* buffer,
                                       size_t size)
{
  // A handle of our own, opened for this read. Deliberately not one a worker
  // holds: behind a worker stuck in a read, a fallback that shared its handle
  // would block too, and then it would not be a fallback.
  auto source = m_factory(path);
  if (!source)
    return -1;
  return source->ReadAt(offset, buffer, size);
}

CBlurayStreamCache::Stats CBlurayStreamCache::GetStats() const
{
  Stats stats;
  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    stats = m_stats;
  }
  FileState* const head = m_head.load(std::memory_order_acquire);
  const int64_t headOffset = m_headOffset.load(std::memory_order_acquire);
  stats.headOffset = static_cast<uint64_t>(std::max<int64_t>(0, headOffset));

  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    stats.filesRetained = m_files.size();
    stats.pagesResidentNow = 0;
    for (const auto& file : m_files)
      stats.pagesResidentNow += file.second->pages.size();

    // Contiguous resident bytes ahead of the read head. Contiguous is the
    // point: a gap is where playback would stop, so counting every resident
    // page ahead would overstate the cover by whatever sits past the gap.
    if (head)
    {
      stats.headFile = NameOf(head->path);
      int64_t page = headOffset / static_cast<int64_t>(m_config.pageSize);
      const int64_t into = headOffset % static_cast<int64_t>(m_config.pageSize);
      int64_t ahead = 0;
      auto it = head->pages.find(page);
      if (it != head->pages.end() && static_cast<int64_t>(it->second.validBytes) > into)
      {
        ahead = static_cast<int64_t>(it->second.validBytes) - into;
        while (true)
        {
          auto next = head->pages.find(++page);
          if (next == head->pages.end())
            break;
          ahead += static_cast<int64_t>(next->second.validBytes);
        }
      }
      stats.bytesAhead = static_cast<uint64_t>(ahead);

      // Everything resident at or past the head, gaps included.
      const int64_t headPage = headOffset / static_cast<int64_t>(m_config.pageSize);
      int64_t total = 0;
      for (const auto& entry : head->pages)
        if (entry.first >= headPage)
          total += static_cast<int64_t>(entry.second.validBytes);
      stats.bytesAheadTotal = static_cast<uint64_t>(total);
    }
  }

  // Divided by a ROLLING rate over the last 10-20 s, not the session mean.
  double bytesPerMs = 0.0;
  {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lock(m_rateMutex);
    if (m_rateMarkMs == 0)
    {
      m_rateMarkMs = m_ratePrevMs = now;
      m_rateMarkBytes = m_ratePrevBytes = stats.bytes;
    }
    else if ((now - m_rateMarkMs) >= 10000)
    {
      // Rotate: the older mark is what the rate is measured from, so the
      // window never collapses to zero right after a rotation.
      m_ratePrevMs = m_rateMarkMs;
      m_ratePrevBytes = m_rateMarkBytes;
      m_rateMarkMs = now;
      m_rateMarkBytes = stats.bytes;
    }
    const int64_t span = now - m_ratePrevMs;
    if (span > 0 && stats.bytes >= m_ratePrevBytes)
      bytesPerMs = static_cast<double>(stats.bytes - m_ratePrevBytes) / static_cast<double>(span);
  }

  // Before the first window closes, fall back to the session mean rather than
  // report nothing.
  if (bytesPerMs <= 0.0)
  {
    const int64_t elapsed = NowMs() - m_startedMs;
    if (elapsed > 0 && stats.bytes > 0)
      bytesPerMs = static_cast<double>(stats.bytes) / static_cast<double>(elapsed);
  }

  if (bytesPerMs > 0.0)
  {
    stats.rateBytesPerSec = static_cast<uint64_t>(bytesPerMs * 1000.0);
    stats.coverMs = static_cast<uint64_t>(static_cast<double>(stats.bytesAhead) / bytesPerMs);
    stats.coverTotalMs =
        static_cast<uint64_t>(static_cast<double>(stats.bytesAheadTotal) / bytesPerMs);
  }

  return stats;
}

std::string CBlurayStreamCache::GetOverlayLine()
{
  {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    // 500 ms is well under human reading speed and keeps the page walk off
    // the render path.
    if (!m_overlayLine.empty() && (NowMs() - m_overlayAtMs) < 500)
      return m_overlayLine;
  }

  const Stats s = GetStats();
  const double hit = s.pagesRequested
                         ? 100.0 * static_cast<double>(s.pagesResidentOnArrival) /
                               static_cast<double>(s.pagesRequested)
                         : 100.0;
  // Waits are split because they are not equal: the sub-10ms ones are the
  // reader brushing a page already in flight, and only the slow ones are felt.
  const uint64_t slow = s.waits100ms;
  // snprintf, not StringUtils::Format: everything in this file is kept free of
  // the rest of Kodi so the class stays testable against a fake source.
  //
  // Compact on purpose. This sits on the player's format line beside the
  // codec, aspect and bitrate, which is already long on a 4K DV title, so it
  // carries only what is worth reading at a glance: how much is held, what
  // that is worth in seconds at the current rate, the hit rate, and the waits
  // that actually cost something (>=100 ms - about nine in ten waits are
  // sub-10 ms brushes against a page already in flight). The full breakdown
  // stays in the log line.
  char buf[128];
  std::snprintf(buf, sizeof(buf), "RA %lluMB/%.0fs %.2f%%%s",
                static_cast<unsigned long long>(s.bytesAheadTotal / (1024 * 1024)),
                s.coverTotalMs / 1000.0, hit,
                slow ? (" " + std::to_string(slow) + " slow").c_str() : "");
  std::string line(buf);

  std::lock_guard<std::mutex> lock(m_overlayMutex);
  m_overlayLine = line;
  m_overlayAtMs = NowMs();
  return line;
}

void CBlurayStreamCache::GetCacheStatus(uint64_t& forward, uint64_t& maxforward,
                                        uint32_t& currate)
{
  const Stats s = GetStats();
  forward = s.bytesAheadTotal;
  maxforward = m_arenaBytes;
  const int64_t elapsed = NowMs() - m_startedMs;
  currate = (elapsed > 0) ? static_cast<uint32_t>(s.bytes * 1000 / elapsed) : 0;
}

void CBlurayStreamCache::MaybeLogStats()
{
  const int64_t now = NowMs();
  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    if ((now - m_lastStatsLogMs) < STATS_LOG_INTERVAL_MS)
      return;
    m_lastStatsLogMs = now;
  }
  LogStats("playing");
}

void CBlurayStreamCache::LogStats(const char* reason)
{
  const Stats s = GetStats();

  // One line, LOGINFO so it survives the box's log level, and shaped so the
  // numbers that matter can be grepped out of a run: cover first, because
  // that is what says an outage is survivable.
  CLog::Log(LOGINFO,
            "CBlurayStreamCache::LogStats - {}: head {}@{} MB | cover {} MB / {} ms contiguous, "
            "{} MB / {} ms resident | "
            "reads {} ({} waited, "
            "{} bytes) | pages {} requested = {} resident + {} waited + {} fail-open, {} fetched "
            "({} failed, {} retries) | waits >=10ms {} >=100ms {} >=1s {}, max {} ms, longest "
            "stall ridden {} ms | seeks {} in window / {} out | {} evictions, {} fail-open reads, "
            "{} workers abandoned | primes {} started / {} adopted / {} dropped, {} pages "
            "({} same-clip started / {} adopted) | "
            "waits: {} first-of-title, {} first-of-clip ({} primed / {} unprimed), "
            "{} after-seek, {} sequential | "
            "{} files retained, {} pages resident",
            reason, s.headFile.empty() ? "-" : s.headFile.c_str(),
            s.headOffset / (1024 * 1024), s.bytesAhead / (1024 * 1024), s.coverMs,
            s.bytesAheadTotal / (1024 * 1024), s.coverTotalMs, s.reads,
            s.readsWaited, s.bytes,
            s.pagesRequested, s.pagesResidentOnArrival, s.pagesWaited, s.pagesFailOpen,
            s.pagesFetched, s.pageFailures, s.retries, s.waits10ms, s.waits100ms, s.waits1000ms,
            s.maxWaitMs, s.longestStallRiddenMs, s.seeksInWindow, s.seeksOutOfWindow, s.evictions,
            s.failOpenReads, s.workersAbandoned, s.primesStarted, s.primesAdopted,
            s.primesDropped, s.primePagesFetched, s.sameClipPrimesStarted,
            s.sameClipPrimesAdopted, s.waitsFirstReadOfTitle,
            s.waitsFirstReadOfFile, s.waitsFirstReadPrimed, s.waitsFirstReadUnprimed,
            s.waitsAfterSeek, s.waitsSequential, s.filesRetained,
            s.pagesResidentNow);
}

CBlurayStreamCache::CView::CView(std::shared_ptr<CBlurayStreamCache> cache,
                                 std::string path,
                                 int64_t length)
  : m_cache(std::move(cache)), m_path(std::move(path)), m_length(length)
{
}

int64_t CBlurayStreamCache::CView::Read(uint8_t* buffer, size_t size)
{
  const int64_t n = m_cache->ReadAt(m_path, m_position, buffer, size);
  if (n > 0)
    m_position += n;
  return n;
}

int64_t CBlurayStreamCache::CView::Seek(int64_t offset, int whence)
{
  int64_t target = offset;
  if (whence == SEEK_CUR)
    target = m_position + offset;
  else if (whence == SEEK_END)
    target = m_length + offset;

  if (target < 0)
    return -1;
  m_position = target;
  return m_position;
}
