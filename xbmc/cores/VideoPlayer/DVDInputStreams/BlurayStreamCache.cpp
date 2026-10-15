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

int64_t NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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
  // The window has to leave room for what is kept behind, or it evicts the
  // pages it just fetched and every one of them is read twice.
  m_windowPages = m_pageCount > (behindPages + 1) ? m_pageCount - behindPages - 1 : 1;
  return true;
}

CBlurayStreamCache::~CBlurayStreamCache()
{
  Abort();
  LogStats("close");

  {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_stopWorker = true;
    m_window.clear();
    m_queued.clear();

    if (m_workerCtx && m_workerCtx->readStartMs.load() != 0 && m_abandoned < m_config.abandonMax)
    {
      // Stuck in a source read. Leave it running rather than make the close
      // wait out the network: it owns its own handle and buffer, and once
      // `owner` is cleared it cannot touch anything of ours again.
      AbandonWorkerLocked(lock, "close");
    }
  }
  m_queueChanged.notify_all();

  if (m_worker.joinable())
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
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_files.find(path);
    if (it != m_files.end())
      length = it->second->length;
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

  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.reads;
  }

  size_t done = 0;
  bool servedWholeFromCache = true;
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
          if (shortPage && copied == 0)
            return done > 0 ? static_cast<int64_t>(done) : -1;
          break;
        }

        if (file->failedPages.erase(pageIndex) != 0)
        {
          // The worker gave up on this page. Read it here instead; the mark is
          // already cleared, so the next pass over it tries the cache again.
          readDirect = true;
          break;
        }

        if (!waited)
        {
          std::lock_guard<std::mutex> statsLock(m_statsMutex);
          ++m_stats.pageMisses;
        }

        // Not resident: make sure it is queued, then wait for it rather than
        // reading it on this thread. This is the whole ride-through: the
        // thread that must stay responsive never enters a network read.
        lock.unlock();
        QueuePage(file, pageIndex);
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
        const uint64_t ms = static_cast<uint64_t>(std::max<int64_t>(0, NowMs() - waitStart));
        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        ++m_stats.waits;
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
      servedWholeFromCache = false;
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
  }

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

  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.pageHits;
  }
  shortPage = (offset + static_cast<int64_t>(chunk)) >= pageEnd &&
              it->second.validBytes < m_config.pageSize;
  return chunk;
}

void CBlurayStreamCache::InsertPageUnlocked(const std::shared_ptr<FileState>& file,
                                            int64_t pageIndex,
                                            const uint8_t* data,
                                            size_t validBytes)
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
  m_lru.push_front({file.get(), pageIndex});
  file->pages.emplace(pageIndex, Resident{page, validBytes, m_lru.begin()});
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

  // Least recently used, which is the eviction order the design asks for
  // without a second mechanism: pages behind the reader - and a closed clip's
  // retained pages - are the ones that stopped being touched, while the
  // forward window was only just filled.
  while (!m_lru.empty())
  {
    auto victim = m_lru.back();
    FileState* file = victim.first;
    auto fileIt = file->pages.find(victim.second);
    m_lru.pop_back();
    if (fileIt == file->pages.end())
      continue;

    page = fileIt->second.data;
    file->pages.erase(fileIt);
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.evictions;
    return true;
  }

  return false;
}

void CBlurayStreamCache::QueueForwardWindow(const std::shared_ptr<FileState>& file, int64_t lastPage)
{
  const int64_t maxPage = (file->length - 1) / static_cast<int64_t>(m_config.pageSize);
  const int64_t windowEnd =
      std::min<int64_t>(maxPage, lastPage + static_cast<int64_t>(m_windowPages));

  int64_t queueFrom = 0;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    // Only read ahead for a reader walking forward. libbluray interleaves
    // index/clip lookups with the stream, and reading ahead of one of those
    // pulls a window nobody wants. An unknown position - start of play, or
    // just after a jump - establishes the anchor; the next read prefetches.
    if (file->lastReadEndPage < 0 || lastPage < file->lastReadEndPage ||
        lastPage > file->lastReadEndPage + 1)
    {
      file->lastReadEndPage = lastPage;
      file->prefetchHorizon = -1;
      return;
    }

    file->lastReadEndPage = lastPage;
    queueFrom = std::max<int64_t>(lastPage + 1, file->prefetchHorizon + 1);
    if (queueFrom > windowEnd)
      return;
    file->prefetchHorizon = windowEnd;
  }

  for (int64_t page = queueFrom; page <= windowEnd; ++page)
    QueuePage(file, page);
}

void CBlurayStreamCache::QueuePage(const std::shared_ptr<FileState>& file, int64_t pageIndex)
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
      return;
    // Always the back: the queue is ordered by distance ahead of the reader,
    // so the nearest outstanding page stays at the head, where the worker
    // pops. Feeding the far end first starves the very next read.
    m_window.push_back(Work{file, pageIndex});
  }
  m_queueChanged.notify_one();
}

void CBlurayStreamCache::StartWorker()
{
  auto ctx = std::make_shared<WorkerCtx>();
  ctx->owner = this;
  ctx->buffer.resize(m_config.pageSize);

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
  if (m_stopWorker || !m_workerCtx || m_abandoned >= m_config.abandonMax)
    return false;

  const int64_t started = m_workerCtx->readStartMs.load();
  if (started == 0 || (NowMs() - started) < static_cast<int64_t>(m_config.abandonAfterMs))
    return false;

  AbandonWorkerLocked(lock, "stalled");

  // A replacement, so the window fills again as soon as the link returns.
  auto ctx = std::make_shared<WorkerCtx>();
  ctx->owner = this;
  ctx->buffer.resize(m_config.pageSize);
  m_workerCtx = ctx;
  m_worker = std::thread(&CBlurayStreamCache::Worker, ctx);
  return true;
}

void CBlurayStreamCache::AbandonWorkerLocked(std::unique_lock<std::mutex>& queueLock,
                                             const char* reason)
{
  auto ctx = m_workerCtx;
  m_workerCtx.reset();
  ++m_abandoned;

  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    // Cleared first: from here the thread cannot reach this cache at all, so
    // it is safe for it to outlive us.
    ctx->owner = nullptr;
    ctx->cancel.store(true);
  }

  if (m_worker.joinable())
    m_worker.detach();

  {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.workersAbandoned;
  }

  queueLock.unlock();
  CLog::Log(LOGINFO,
            "CBlurayStreamCache - worker abandoned ({}), it is stuck in a source read; {} of {} "
            "used",
            reason, m_abandoned, m_config.abandonMax);
  queueLock.lock();
}

void CBlurayStreamCache::Worker(std::shared_ptr<WorkerCtx> ctx)
{
  while (!ctx->cancel.load())
  {
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
}

CBlurayStreamCache::ISource* CBlurayStreamCache::GetWorkerSource(WorkerCtx& ctx,
                                                                 const std::string& path)
{
  auto it = ctx.sources.find(path);
  if (it != ctx.sources.end())
    return it->second.get();

  // Two is enough for the current clip and the one being primed; anything
  // older is a retained file nobody is reading.
  if (ctx.sources.size() >= 2)
    ctx.sources.clear();

  auto source = m_factory(path);
  if (!source)
    return nullptr;
  auto* raw = source.get();
  ctx.sources.emplace(path, std::move(source));
  return raw;
}

bool CBlurayStreamCache::RunOneWorkItem(WorkerCtx& ctx)
{
  Work work;
  {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_queueChanged.wait(lock, [&] {
      return m_stopWorker || ctx.cancel.load() || m_workerCtx.get() != &ctx || !m_window.empty();
    });
    if (m_stopWorker || ctx.cancel.load() || m_workerCtx.get() != &ctx)
      return false;

    work = m_window.front();
    m_window.pop_front();
    m_queued.erase(WorkKey(work.file->path, work.pageIndex));
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
  for (unsigned int attempt = 0; attempt < PAGE_READ_ATTEMPTS && !ctx.cancel.load(); ++attempt)
  {
    ISource* source = GetWorkerSource(ctx, work.file->path);
    if (!source)
      break;

    // The watchdog reads this without the lock to spot a wedged read.
    ctx.readStartMs.store(NowMs());
    read = source->ReadAt(offset, ctx.buffer.data(), bytesToRead);
    ctx.readStartMs.store(0);

    if (read == static_cast<int64_t>(bytesToRead))
      break;

    // Short or failed. Drop the handle - on this link that is usually a
    // transient the next open rides through - and back off before retrying,
    // rather than reporting it as the end of the file.
    ctx.sources.erase(work.file->path);
    read = -1;
    if (attempt + 1 < PAGE_READ_ATTEMPTS)
      std::this_thread::sleep_for(std::chrono::milliseconds(PAGE_RETRY_BASE_MS << attempt));
  }

  {
    std::lock_guard<std::mutex> ctxLock(ctx.mutex);
    if (!ctx.owner)
      return false; // abandoned while we were in the read; touch nothing
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
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    stats.filesRetained = m_files.size();
    stats.pagesResident = 0;
    for (const auto& file : m_files)
      stats.pagesResident += file.second->pages.size();
  }
  return stats;
}

void CBlurayStreamCache::LogStats(const char* reason)
{
  const Stats s = GetStats();
  CLog::Log(LOGINFO,
            "CBlurayStreamCache::LogStats - {}: {} reads ({} bytes), {} page hits / {} misses, {} "
            "pages fetched ({} failed), {} waits ({} >=10ms, {} >=100ms, {} >=1s, max {} ms), "
            "seeks {} in window / {} out, {} evictions, {} fail-open reads, {} workers abandoned, "
            "{} files retained, {} pages resident",
            reason, s.reads, s.bytes, s.pageHits, s.pageMisses, s.pagesFetched, s.pageFailures,
            s.waits, s.waits10ms, s.waits100ms, s.waits1000ms, s.maxWaitMs, s.seeksInWindow,
            s.seeksOutOfWindow, s.evictions, s.failOpenReads, s.workersAbandoned, s.filesRetained,
            s.pagesResident);
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
