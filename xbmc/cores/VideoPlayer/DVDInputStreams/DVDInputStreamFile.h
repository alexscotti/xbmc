/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BlurayStreamCache.h"
#include "DVDInputStream.h"

#include <atomic>
#include <memory>
#include <mutex>

class CDVDInputStreamFile : public CDVDInputStream
{
public:
  explicit CDVDInputStreamFile(const CFileItem& fileitem, unsigned int flags);
  ~CDVDInputStreamFile() override;
  bool Open() override;
  void Close() override;
  int Read(uint8_t* buf, int buf_size) override;
  int64_t Seek(int64_t offset, int whence) override;
  bool IsEOF() override;
  void Abort() override;
  std::string GetReadAheadInfo() override;
  int64_t GetLength() override;
  BitstreamStats GetBitstreamStats() const override ;
  int GetBlockSize() override;
  void SetReadRate(uint32_t rate) override;
  bool GetCacheStatus(XFILE::SCacheStatus *status) override;

protected:
  XFILE::CFile* m_pFile = nullptr;
  bool m_eof = false;
  unsigned int m_flags = 0;

  // Read-ahead for a single video file on a network share, in place of
  // CFileCache (plan section 1b). When this is set, m_pFile is closed at the
  // end of Open and is null: the cache owns its own handles, so a Stop while
  // the share is away does not have to wait for a cache thread stuck in a
  // read, which is what CFileCache's Close does today - nor for a close of a
  // handle of ours, which on smb:// waits on the same lock that read holds.
  // Written by the player thread, read by Abort() on the closing thread and
  // GetReadAheadInfo() on the render thread: replaced and read under the lock.
  std::shared_ptr<CBlurayStreamCache> m_streamCache;
  mutable std::mutex m_streamCacheMutex;
  std::atomic<bool> m_abortRequested{false};
  void SetStreamCache(std::shared_ptr<CBlurayStreamCache> cache);
  std::shared_ptr<CBlurayStreamCache> StreamCache() const;
  std::shared_ptr<CBlurayStreamCache::CView> m_streamView;
  // What m_pFile answered before it was closed.
  int64_t m_seekPossible = 0;
  int m_chunkSize = 0;
};
