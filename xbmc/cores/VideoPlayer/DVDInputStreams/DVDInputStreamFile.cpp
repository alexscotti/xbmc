/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDInputStreamFile.h"

#include "StreamCacheSetup.h"

#include "URL.h"
#include "filesystem/File.h"
#include "filesystem/IFile.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "video/VideoFileItemClassify.h"

using namespace KODI;
using namespace XFILE;

CDVDInputStreamFile::CDVDInputStreamFile(const CFileItem& fileitem, unsigned int flags)
  : CDVDInputStream(DVDSTREAM_TYPE_FILE, fileitem), m_flags(flags)
{
  m_pFile = NULL;
  m_eof = true;
}

CDVDInputStreamFile::~CDVDInputStreamFile()
{
  Close();
}

std::string CDVDInputStreamFile::GetReadAheadInfo()
{
  if (m_streamCache)
    return m_streamCache->GetOverlayLine();
  return {};
}

void CDVDInputStreamFile::Abort()
{
  // Called when the player is tearing down. Every reader waiting on a page
  // returns at once, so a Stop while the share is away does not wait out the
  // network - the thing CFileCache's Close cannot do.
  if (m_streamCache)
    m_streamCache->Abort();
}

bool CDVDInputStreamFile::IsEOF()
{
  return !m_pFile || m_eof;
}

bool CDVDInputStreamFile::Open()
{
  if (!CDVDInputStream::Open())
    return false;

  m_pFile = new CFile();
  if (!m_pFile)
    return false;

  unsigned int flags = m_flags;

  // If this file is audio and/or video (= not a subtitle) flag to caller
  if (!VIDEO::IsSubtitle(m_item))
    flags |= READ_AUDIO_VIDEO;
  else
    flags |= READ_NO_BUFFER; // disable CFileStreamBuffer for subtitles

  std::string content = m_item.GetMimeType();

  if (content == "video/mp4" ||
      content == "video/x-msvideo" ||
      content == "video/avi" ||
      content == "video/x-matroska" ||
      content == "video/x-matroska-3d")
    flags |= READ_MULTI_STREAM;

  // Read-ahead in place of CFileCache, for a single video file on a network
  // share. The gate is deliberately three things, not two: the caller must
  // not already have asked for READ_NO_CACHE, because this same class also
  // carries a Blu-ray ISO's own stream (opened READ_TRUNCATED | READ_BITRATE
  // | READ_NO_CACHE by CDVDInputStreamBluray), and caching that here would
  // put a stream cache underneath a CBlurayIsoCache.
  const std::string path = m_item.GetDynPath();
  if (!(m_flags & READ_NO_CACHE) && STREAMCACHE::IsCacheableSingleFile(path) &&
      STREAMCACHE::IsRemotePath(path))
  {
    m_streamCache = STREAMCACHE::ClaimFor(path);
    if (m_streamCache)
    {
      // One buffer, never two: the file itself is opened uncached and the
      // cache sits in front of it.
      flags |= READ_NO_CACHE;
      flags &= ~READ_CACHED;
    }
  }

  // open file in binary mode
  if (!m_pFile->Open(m_item.GetDynPath(), flags))
  {
    delete m_pFile;
    m_pFile = NULL;
    return false;
  }

  if (m_streamCache)
  {
    m_streamView = m_streamCache->OpenFile(path);
    if (!m_streamView)
    {
      // Could not be cached after all; m_pFile is open and uncached, so read
      // through it directly rather than fail the playback.
      CLog::Log(LOGINFO, "CDVDInputStreamFile - read-ahead declined for {}, reading directly",
                CURL::GetRedacted(path));
      m_streamCache.reset();
    }
  }

  if (m_pFile->GetImplementation() && (content.empty() || content == "application/octet-stream"))
    m_content = m_pFile->GetImplementation()->GetProperty(XFILE::FileProperty::CONTENT_TYPE);

  m_eof = false;
  return true;
}

// close file and reset everything
void CDVDInputStreamFile::Close()
{
  // Before m_pFile: the view reads through the cache, not through it.
  m_streamView.reset();
  m_streamCache.reset();

  if (m_pFile)
  {
    m_pFile->Close();
    delete m_pFile;
  }

  CDVDInputStream::Close();
  m_pFile = NULL;
  m_eof = true;
}

int CDVDInputStreamFile::Read(uint8_t* buf, int buf_size)
{
  if (m_streamView)
  {
    if (buf_size <= 0)
      return 0;
    const int64_t n = m_streamView->Read(buf, static_cast<size_t>(buf_size));
    if (n < 0)
      return -1;
    if (n == 0)
      m_eof = true;
    return static_cast<int>(n);
  }

  if(!m_pFile) return -1;

  ssize_t ret = m_pFile->Read(buf, buf_size);

  if (ret < 0)
    return -1; // player will retry read in case of error until playback is stopped

  /* we currently don't support non completing reads */
  if (ret == 0)
    m_eof = true;

  return (int)ret;
}

int64_t CDVDInputStreamFile::Seek(int64_t offset, int whence)
{
  if(!m_pFile) return -1;

  if (whence == DVDSTREAM_SEEK_POSSIBLE)
    return m_pFile->IoControl(IOControl::SEEK_POSSIBLE, nullptr);

  if (m_streamView)
  {
    const int64_t ret = m_streamView->Seek(offset, whence);
    if (ret >= 0)
      m_eof = false;
    return ret;
  }

  int64_t ret = m_pFile->Seek(offset, whence);

  /* if we succeed, we are not eof anymore */
  if( ret >= 0 ) m_eof = false;

  return ret;
}

int64_t CDVDInputStreamFile::GetLength()
{
  if (m_streamView)
    return m_streamView->GetLength();
  if (m_pFile)
    return m_pFile->GetLength();
  return 0;
}

bool CDVDInputStreamFile::GetCacheStatus(XFILE::SCacheStatus *status)
{
  // Same reason as the Blu-ray path: CFileCache is bypassed when the stream
  // cache is in front, so these come from it instead.
  if (status && m_streamCache)
  {
    uint64_t forward = 0;
    uint64_t maxforward = 0;
    uint32_t currate = 0;
    m_streamCache->GetCacheStatus(forward, maxforward, currate);
    status->forward = forward;
    status->maxforward = maxforward;
    status->currate = currate;
    status->maxrate = 0;
    status->lowrate = 0;
    return true;
  }

  if (m_pFile && m_pFile->IoControl(IOControl::CACHE_STATUS, status) >= 0)
    return true;
  else
    return false;
}

BitstreamStats CDVDInputStreamFile::GetBitstreamStats() const
{
  if (!m_pFile)
    return m_stats; // dummy return. defined in CDVDInputStream

  if(m_pFile->GetBitstreamStats())
    return *m_pFile->GetBitstreamStats();
  else
    return m_stats;
}

// Use value returned by filesystem if is > 1
// otherwise defaults to 64K
int CDVDInputStreamFile::GetBlockSize()
{
  int chunk = 0;
  if (m_pFile)
    chunk = m_pFile->GetChunkSize();

  return ((chunk > 1) ? chunk : 64 * 1024);
}

void CDVDInputStreamFile::SetReadRate(uint32_t rate)
{
  // Increase requested rate by 10%:
  uint32_t maxrate = static_cast<uint32_t>(1.1 * rate);

  if (m_pFile->IoControl(IOControl::CACHE_SETRATE, &maxrate) >= 0)
    CLog::Log(LOGDEBUG,
              "CDVDInputStreamFile::SetReadRate - set cache throttle rate to {} bytes per second",
              maxrate);
}
