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
  // CFileCache (plan section 1b). When this is set, m_pFile is not used for
  // reading at all: the cache owns its own handles, so a Stop while the share
  // is away does not have to wait for a cache thread stuck in a read, which
  // is what CFileCache's Close does today.
  std::shared_ptr<CBlurayStreamCache> m_streamCache;
  std::shared_ptr<CBlurayStreamCache::CView> m_streamView;
};
