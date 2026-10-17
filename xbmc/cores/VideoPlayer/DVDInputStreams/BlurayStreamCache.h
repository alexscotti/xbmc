/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A read-ahead cache for the clips of a Blu-ray folder (BDMV/STREAM/*.m2ts) on
// a network filesystem, and for a single network video file.
//
// libbluray opens each clip through file_open -> a bare CFile::Open, with no
// buffering of any kind behind it (CFile caches only DVD/Blu-ray-scheme paths
// or READ_AUDIO_VIDEO opens). The demux queues and the decoder's input hold a
// few seconds, so a NAS stall longer than that freezes the film, and a Stop
// during the stall freezes the GUI for as long as the blocking read takes -
// 9.8 / 24.3 / 25.1 s measured for 11-20 s outages.
//
// What this class does about it:
//
//  - It is SESSION level, not per clip. One cache per playing title owns the
//    arena, the worker and the stats; its page map is keyed by (path, page).
//    A clip open is a view onto it, not an allocation, and a closed clip's
//    pages stay resident - so a back-seek across a clip boundary is served
//    from memory rather than re-read from the NAS.
//  - The reader never blocks on the network. It waits on a condition variable
//    for a page a worker is fetching, so an outage shorter than the buffered
//    time is invisible and Abort() returns every waiter at once.
//  - A worker blocked in a source read is never joined. It owns its own
//    source handle and a private buffer, copies into the arena only under the
//    cache lock and only while its session is live, and is abandoned (and
//    replaced) rather than waited for - at close, and mid-session once it has
//    been stuck longer than the abandon threshold.
//  - Memory is a dedicated arena, allocated and pre-faulted once, carved into
//    fixed pages. A worker never owns an arena page, so a worker abandoned in
//    a long outage cannot write into memory the next title already owns.
//
// Everything the source does is behind ISource, so the whole class is testable
// against a fake without CFile, libbluray or a network.
class CBlurayStreamCache : public std::enable_shared_from_this<CBlurayStreamCache>
{
public:
  // One open file the cache reads from. Each worker and each fail-open
  // fallback gets its own, so two reads can be in flight at once and a read
  // that is stuck cannot hold up a read that is not.
  class ISource
  {
  public:
    virtual ~ISource() = default;
    virtual int64_t GetLength() = 0;
    // Absolute offset. Returns bytes read, 0 at EOF, < 0 on error.
    virtual int64_t ReadAt(int64_t offset, uint8_t* buffer, size_t size) = 0;
  };

  // Opens `path`, or returns nullptr. Called from the reader's thread and from
  // workers, so it must be safe to call concurrently.
  using SourceFactory = std::function<std::unique_ptr<ISource>(const std::string& path)>;

  struct Config
  {
    size_t arenaBytes{0}; // 0 disables the cache entirely
    size_t pageSize{1024 * 1024};
    size_t behindBytes{8 * 1024 * 1024};
    // A worker stuck in one source read for longer than this is abandoned and
    // replaced, so the window starts filling again as soon as the link does.
    unsigned int abandonAfterMs{5000};
    // Bound on threads left running on a dead session. Past it, close waits.
    unsigned int abandonMax{4};
    // Within this much of a clip's end, start pulling the head of the next
    // clip; this much of it. 0 disables chaining.
    size_t nextLeadBytes{32 * 1024 * 1024};
    size_t nextSizeBytes{32 * 1024 * 1024};
  };

  // One session at a time gets read-ahead: the arena has a single owner,
  // claimed here and released when the returned cache is destroyed. A second
  // concurrent session - and a network .mkv draws from the same arena, so this
  // is one rule across both paths - gets nullptr and reads directly, which is
  // the behaviour without this class at all.
  static std::shared_ptr<CBlurayStreamCache> Claim(const Config& config, SourceFactory factory);

  ~CBlurayStreamCache();

  CBlurayStreamCache(const CBlurayStreamCache&) = delete;
  CBlurayStreamCache& operator=(const CBlurayStreamCache&) = delete;

  // What libbluray's file_open and the single-file input stream hold. Carries
  // its own file position; the pages behind it belong to the session.
  class CView
  {
  public:
    CView(std::shared_ptr<CBlurayStreamCache> cache, std::string path, int64_t length);

    int64_t Read(uint8_t* buffer, size_t size);
    int64_t Seek(int64_t offset, int whence);
    int64_t GetPosition() const { return m_position; }
    int64_t GetLength() const { return m_length; }
    bool AtEof() const { return m_position >= m_length; }
    const std::string& GetPath() const { return m_path; }

  private:
    std::shared_ptr<CBlurayStreamCache> m_cache;
    std::string m_path;
    int64_t m_length{0};
    int64_t m_position{0};
  };

  // nullptr when the file cannot be opened; the caller then reads it directly.
  // A file a prime has already touched is opened WITHOUT going to the source
  // at all - its length is known and its head is resident - which is how the
  // blocking open at a clip boundary is avoided.
  std::shared_ptr<CView> OpenFile(const std::string& path);

  // clip -> next clip for the title being played, from the playlist libbluray
  // is actually playing. Republishing drops a prime that is no longer next
  // (an angle change, a navigation jump), so nothing stale is ever adopted.
  // An empty nextPath means "nothing follows": the end of the playlist.
  // nextOffset is where the next playitem begins in nextPath: 0 for the usual
  // next clip, and that playitem's first source packet when it shares a clip
  // with the current one. currentEndOffset is where the CURRENT playitem ends
  // (-1 = at the end of the file), which is the boundary the lead is measured
  // against - a same-clip seam is not an EOF.
  void SetChainHint(const std::string& currentPath,
                    const std::string& nextPath,
                    int64_t nextOffset = 0,
                    int64_t currentEndOffset = -1);

  // Returns every waiting reader at once and fails every pending read. Called
  // from CDVDInputStreamBluray::Abort() before the player waits for its
  // threads, so a Stop during an outage does not wait out the network.
  void Abort();

  bool IsAborted() const { return m_aborted; }

  void LogStats(const char* reason);

  // One short line for the on-screen codec-info overlay: what is held, the
  // hit rate, and the waits that actually cost something. Rate limited and
  // cached, because the overlay redraws every frame while GetStats() walks
  // the page map under the cache lock.
  std::string GetOverlayLine();

  // Fills the fields Kodi's player overlay already has for CFileCache, so
  // "forward: ... / ...% / ...s" keeps working on a path where CFileCache is
  // deliberately bypassed and would otherwise read as empty.
  void GetCacheStatus(uint64_t& forward, uint64_t& maxforward, uint32_t& currate);
  // Logs the stats if it has been longer than the interval, so a long film
  // reports while it plays instead of only at the stop. Called off the read
  // path with no cache lock held.
  void MaybeLogStats();

  // Counters are deliberately split by unit, and never overlap: a page access
  // is counted as resident-on-arrival OR waited-for, never both. The earlier
  // pair did overlap - a page the reader waited for was then copied, which
  // counted a hit as well - and mixed per-read with per-page totals, so the
  // "hit rate" it implied was meaningless.
  struct Stats
  {
    // per read request from libbluray / the input stream
    uint64_t reads{0};
    uint64_t readsWaited{0}; // waited on at least one page: the miss rate
    uint64_t bytes{0};

    // per distinct page access by the reader. pagesRequested ==
    // pagesResidentOnArrival + pagesWaited + pagesFailOpen.
    uint64_t pagesRequested{0};
    uint64_t pagesResidentOnArrival{0};
    uint64_t pagesWaited{0};
    uint64_t pagesFailOpen{0};

    // WHERE the waits happen. A sequential title should wait essentially once
    // - its very first read - and never again: a clip boundary is covered by
    // the prime, and the forward window covers the rest. Cumulative totals
    // cannot tell "one per clip boundary" from a real ongoing shortfall, and
    // those have different fixes, so each wait is attributed.
    uint64_t waitsFirstReadOfTitle{0};
    uint64_t waitsFirstReadOfFile{0};  // a clip the prime did not cover
    // Split of the above: was that clip primed at all? "primed but still
    // waited" and "never primed" need different fixes, and guessing which
    // dominates has already cost a build cycle.
    uint64_t waitsFirstReadPrimed{0};
    uint64_t waitsFirstReadUnprimed{0};
    uint64_t waitsAfterSeek{0};        // the read was not sequential
    uint64_t waitsSequential{0};       // mid-run: the window fell behind

    // what the worker did
    uint64_t pagesFetched{0};
    uint64_t pageFailures{0};
    uint64_t retries{0};

    uint64_t waits10ms{0};
    uint64_t waits100ms{0};
    uint64_t waits1000ms{0};
    uint64_t maxWaitMs{0};
    uint64_t longestStallRiddenMs{0};

    uint64_t seeksInWindow{0};
    uint64_t seeksOutOfWindow{0};

    uint64_t evictions{0};
    uint64_t failOpenReads{0};
    uint64_t workersAbandoned{0};
    uint64_t primesStarted{0};
    uint64_t primesAdopted{0};
    uint64_t primesDropped{0};
    uint64_t primePagesFetched{0};

    // sampled at the moment the stats are taken, not accumulated
    uint64_t filesRetained{0};
    uint64_t pagesResidentNow{0};
    // Contiguous resident bytes ahead of the read head, and what that is
    // worth in milliseconds at the rate this session has actually been read
    // at. This is the number that says whether an outage is survivable; no
    // hit rate can, because a page fetched just in time counts as a hit.
    uint64_t bytesAhead{0};
    uint64_t coverMs{0};
    // bytesAhead above is the CONTIGUOUS run from the head, which is what a
    // reader can consume without a gap. On its own it is misleading: a player
    // that reads two regions at once (measured at fetched/read = 0.49x on The
    // Conversation) leaves the resident set fragmented, and the contiguous run
    // then reports 24 MB while all 256 pages of the arena are resident. These
    // two say how much is really held, so a full arena can never look empty.
    uint64_t bytesAheadTotal{0}; // resident bytes at or past the head, gaps and all
    uint64_t coverTotalMs{0};
    // The rate the cover seconds are divided by, so the figure can be
    // checked rather than trusted.
    uint64_t rateBytesPerSec{0};
    // Where the reader actually is. Without this the log cannot distinguish a
    // reader advancing through a file from one re-reading the same region, and
    // those produce identical read/byte counts with very different fetch
    // counts - which is exactly the ambiguity that made a frozen pagesFetched
    // unexplainable from the log alone.
    uint64_t headOffset{0};
    std::string headFile;
  };

  Stats GetStats() const;

private:
  struct FileState;

  // Carries one worker's own state. The worker thread holds a shared_ptr to
  // it, so abandoning the worker costs nothing and touches nothing of ours:
  // `owner` is cleared under `mutex` first, and the thread only reaches the
  // cache while it is set.
  struct WorkerCtx
  {
    std::mutex mutex;
    CBlurayStreamCache* owner{nullptr};
    std::atomic<bool> cancel{false};
    // When the current source read started, in steady-clock ms, or 0 for "not
    // in a read". Read by the watchdog without the lock.
    std::atomic<int64_t> readStartMs{0};
    std::vector<uint8_t> buffer;
    // Sources this worker has open, by path. Lazily opened, and only this
    // thread ever touches them.
    std::unordered_map<std::string, std::unique_ptr<ISource>> sources;
  };

  struct Resident
  {
    uint8_t* data{nullptr};
    size_t validBytes{0};
    std::list<std::pair<FileState*, int64_t>>::iterator lruIt;
  };

  struct FileState
  {
    std::string path;
    int64_t length{0};
    std::unordered_map<int64_t, Resident> pages;
    // Pages the worker gave up on after its retries. The reader clears the
    // mark and reads that page directly, so a fault costs throughput for one
    // read rather than leaving a permanent hole in the file.
    std::unordered_set<int64_t> failedPages;
    // Where the sequential run this file is in reached, and how far ahead has
    // already been queued, so a steady reader queues one page per page
    // consumed instead of re-walking the window.
    int64_t lastReadEndPage{-1};
    int64_t prefetchHorizon{-1};
    // Has a reader ever read from this file? A primed clip that has not been
    // adopted yet has not; a clip that was played and closed has. That one
    // bit separates the two for eviction without any extra bookkeeping.
    bool everRead{false};
  };

  struct Work
  {
    std::shared_ptr<FileState> file;
    int64_t pageIndex{0};
  };

  CBlurayStreamCache(const Config& config, SourceFactory factory);
  bool AllocateArena();

  int64_t ReadAt(const std::string& path, int64_t offset, uint8_t* buffer, size_t size);
  std::shared_ptr<FileState> GetFileUnlocked(const std::string& path);

  // Copies out of a resident page. Called with m_cacheMutex held, which is
  // also the only place arena bytes are written, so nothing can be evicted
  // from under a copy and no page needs pinning.
  size_t CopyFromPageUnlocked(FileState& file,
                              int64_t pageIndex,
                              int64_t offset,
                              uint8_t* buffer,
                              size_t size,
                              bool& shortPage);

  void InsertPageUnlocked(const std::shared_ptr<FileState>& file,
                          int64_t pageIndex,
                          const uint8_t* data,
                          size_t validBytes,
                          bool prime = false);

  // What a resident page is worth when the arena is full. Ranked, lowest
  // evicted first: a back-seek is rare, so what the reader has already passed
  // goes before a prime, and the prime goes before the window the reader is
  // about to walk into. Computed at eviction time from the read head rather
  // than tracked, so a page that changes category - a prime once it is being
  // read, a window page once the reader passes it - needs no bookkeeping.
  enum class PageClass
  {
    Behind = 0, // the reader has gone past it
    Prime = 1,  // a clip nobody is reading yet
    Window = 2, // ahead of the reader in the file being read
  };
  PageClass ClassifyUnlocked(const FileState* file, int64_t pageIndex) const;
  void TouchPageUnlocked(FileState& file, int64_t pageIndex);
  bool ReclaimPageUnlocked(uint8_t*& page);
  void QueueForwardWindow(const std::shared_ptr<FileState>& file, int64_t lastPage);
  void QueuePage(const std::shared_ptr<FileState>& file, int64_t pageIndex);
  // Queues the head of the next clip once the reader is within nextLeadBytes
  // of this file's end. Cheap to call on every read: it does nothing until
  // the threshold, and nothing again once that clip is queued.
  void MaybeQueuePrime(const std::shared_ptr<FileState>& file, int64_t offset);
  // Opens the next clip and pulls its head. Yields to window work at every
  // page boundary, so a 32 MB prime cannot hold the link while the clip being
  // watched drains.
  void RunPrime(WorkerCtx& ctx, const std::string& path, int64_t startOffset);

  void StartWorker();
  // Abandons the running worker if it has been stuck longer than the
  // threshold, and starts a replacement. Returns true if it did.
  bool CheckWorkerStall();
  void AbandonWorkerLocked(std::unique_lock<std::mutex>& queueLock, const char* reason);
  static void Worker(std::shared_ptr<WorkerCtx> ctx);
  bool RunOneWorkItem(WorkerCtx& ctx);
  ISource* GetWorkerSource(WorkerCtx& ctx, const std::string& path);

  int64_t ReadDirect(const std::string& path, int64_t offset, uint8_t* buffer, size_t size);

  size_t PageCount() const { return m_pageCount; }
  int64_t PageOf(int64_t offset) const { return offset / static_cast<int64_t>(m_config.pageSize); }

  Config m_config;
  SourceFactory m_factory;

  uint8_t* m_arena{nullptr};
  size_t m_arenaBytes{0};
  size_t m_pageCount{0};
  size_t m_windowPages{1};

  mutable std::mutex m_cacheMutex;
  std::unordered_map<std::string, std::shared_ptr<FileState>> m_files;
  std::vector<uint8_t*> m_freePages;
  std::list<std::pair<FileState*, int64_t>> m_lru; // front = most recently used
  std::condition_variable m_pageChanged;

  std::mutex m_queueMutex;
  std::condition_variable m_queueChanged;
  std::deque<Work> m_window;
  // Second class of work, always served after the window above.
  // A queued prime: which clip, and where in it to start. The start is 0 for
  // the usual next-clip prime and the playitem's first packet for a same-clip
  // seam, so the two share one queue and one worker path.
  struct PrimeJob
  {
    std::string path;
    int64_t offset{0};
    bool operator==(const PrimeJob& other) const
    {
      return path == other.path && offset == other.offset;
    }
  };
  static std::string PrimeKey(const std::string& path, int64_t offset);
  std::deque<PrimeJob> m_primes;
  std::unordered_set<std::string> m_queued;
  std::thread m_worker;
  std::shared_ptr<WorkerCtx> m_workerCtx;
  bool m_stopWorker{false};
  unsigned int m_abandoned{0};

  std::atomic<bool> m_aborted{false};
  // Pages held back from the window for an in-flight prime, so the window
  // cannot oversubscribe the arena and evict the prime it is waiting for.
  size_t m_primeReservePages{0};

  mutable std::mutex m_hintMutex;
  std::string m_hintCurrent;
  std::string m_hintNext;
  int64_t m_hintNextOffset{0};   // where the next playitem starts in m_hintNext
  int64_t m_hintCurrentEnd{-1};  // where the current playitem ends (-1 = EOF)
  std::string m_primeQueuedFor;  // PrimeKey of the outstanding prime

  // A same-clip prime is adopted by a READ landing in it, not by an open:
  // libbluray seeks inside the clip it already has open, so the file_open
  // adoption path never fires for one. Checked on every read, so the common
  // case must cost nothing - these are read unlocked and only then confirmed
  // under m_hintMutex.
  std::atomic<bool> m_sameClipPrime{false};
  std::atomic<int64_t> m_sameClipPrimeOffset{0};

  mutable std::mutex m_statsMutex;
  Stats m_stats;

  std::mutex m_overlayMutex;
  std::string m_overlayLine;
  int64_t m_overlayAtMs{0};

  // A ROLLING read rate, for the cover seconds. The cumulative mean since the
  // session started is the wrong divisor: it includes menu and navigation time
  // at a low or zero rate, so it understates the rate during the feature and
  // overstates how many seconds the buffer is worth - most badly on exactly
  // the high-bitrate passages where the answer matters. Two marks rotated
  // every 10 s give a 10-20 s window.
  mutable std::mutex m_rateMutex;
  mutable int64_t m_rateMarkMs{0};
  mutable uint64_t m_rateMarkBytes{0};
  mutable int64_t m_ratePrevMs{0};
  mutable uint64_t m_ratePrevBytes{0};
  int64_t m_startedMs{0};
  int64_t m_lastStatsLogMs{0};

  // The read head the cover is measured from: the file the reader is in and
  // the offset it has reached. Atomics, not a mutex, because the eviction
  // path reads them while holding m_cacheMutex and must not take a second
  // lock there.
  //
  // A RAW pointer is safe only because of this invariant: a FileState is
  // created by GetFileUnlocked and never removed while the cache lives -
  // m_files is only cleared by the destructor, after the workers are gone.
  // Keep it that way; erasing a FileState mid-session would leave this
  // dangling. (It used to be a shared_ptr read without the lock, with a
  // comment claiming a stale read was harmless. It is not: an unsynchronised
  // read racing an assignment is undefined behaviour, and ClassifyUnlocked
  // dereferences it. ThreadSanitizer caught it in 2 runs out of 12.)
  std::atomic<FileState*> m_head{nullptr};
  std::atomic<int64_t> m_headOffset{0};
};
