/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BlurayStreamCache.h"

#include <memory>
#include <string>

// Everything CBlurayStreamCache needs from the rest of Kodi - CFile, the GUI
// settings, which paths are remote - kept out of the cache itself so the cache
// stays testable against a fake source with no Kodi around it.
namespace STREAMCACHE
{
// A path worth caching: a network filesystem, which is what CFile::Open
// already asks to decide the same thing for CFileCache, or a local path that
// turns out to be a kernel NFS/CIFS mount. Local disks are never remote.
bool IsRemotePath(const std::string& path);

// Sources over CFile, opened READ_NO_CACHE so nothing double-caches.
CBlurayStreamCache::SourceFactory FileSourceFactory();

// Reads the GUI settings. arenaBytes is 0 - the cache disabled - when the
// read-ahead memory setting is 0 or the settings are not up yet.
CBlurayStreamCache::Config ConfigFromSettings();

// A clip of a Blu-ray folder that the cache should serve:
// BDMV/STREAM/<clip>.m2ts. 3D (BDMV/STREAM/SSIF/*.ssif) is deliberately NOT
// cached - its interleaved base+dependent read pattern is nothing like a flat
// clip's and has not been measured - so a 3D disc reads directly, as it does
// today. Everything else libbluray opens (index, mpls, clpi, BD-J jars) is
// small, read once, and left direct.
bool IsCacheableClip(const std::string& relPath);

// A single video file that should get read-ahead in place of CFileCache. Just
// .mkv to begin with - the request was .mkv, and every other container on the
// shelf plays as a BDMV folder - but this is the one list to extend when .ts,
// .m2ts-as-a-file or .mp4 are wanted too.
bool IsCacheableSingleFile(const std::string& path);

// Claims the arena for one session, or returns nullptr - the settings have it
// off, the path is local, or another session already holds it. A nullptr is
// not an error: the caller reads directly, exactly as it does without this.
std::shared_ptr<CBlurayStreamCache> ClaimFor(const std::string& path);
} // namespace STREAMCACHE
