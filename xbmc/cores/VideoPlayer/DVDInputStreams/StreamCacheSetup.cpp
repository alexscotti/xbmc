/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "StreamCacheSetup.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "filesystem/File.h"
#include "filesystem/IFileTypes.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <utility>

#if defined(TARGET_LINUX)
#include <sys/vfs.h>
#endif

using namespace XFILE;

namespace
{
class CFileSource : public CBlurayStreamCache::ISource
{
public:
  explicit CFileSource(std::unique_ptr<CFile> file) : m_file(std::move(file)) {}

  int64_t GetLength() override { return m_file->GetLength(); }

  int64_t ReadAt(int64_t offset, uint8_t* buffer, size_t size) override
  {
    if (m_file->Seek(offset, SEEK_SET) < 0)
      return -1;

    // READ_TRUNCATED, so a Read can come back short; loop it here rather than
    // let a short read look like the end of the file.
    size_t total = 0;
    while (total < size)
    {
      const ssize_t n = m_file->Read(buffer + total, size - total);
      if (n < 0)
        return total > 0 ? static_cast<int64_t>(total) : -1;
      if (n == 0)
        break;
      total += static_cast<size_t>(n);
    }
    return static_cast<int64_t>(total);
  }

private:
  std::unique_ptr<CFile> m_file;
};

#if defined(TARGET_LINUX)
// A user can mount the share in CoreELEC and point Kodi at the mount point,
// and then the path carries no hint of being remote at all.
bool IsKernelNetworkMount(const std::string& path)
{
  constexpr unsigned int NFS_SUPER_MAGIC = 0x6969;
  constexpr unsigned int CIFS_MAGIC_NUMBER = 0xFF534D42;
  constexpr unsigned int SMB2_MAGIC_NUMBER = 0xFE534D42;

  struct statfs st = {};
  if (statfs(path.c_str(), &st) != 0)
    return false;

  const auto type = static_cast<unsigned int>(st.f_type);
  return type == NFS_SUPER_MAGIC || type == CIFS_MAGIC_NUMBER || type == SMB2_MAGIC_NUMBER;
}
#endif
} // namespace

namespace STREAMCACHE
{
bool IsRemotePath(const std::string& path)
{
  if (path.empty())
    return false;

  if (URIUtils::IsNetworkFilesystem(path))
    return true;

  // A FUSE-mounted share reports FUSE's own magic and so reads as local: no
  // cache, which is the safe way to be wrong.
#if defined(TARGET_LINUX)
  if (!URIUtils::IsRemote(path) && path.front() == '/')
    return IsKernelNetworkMount(path);
#endif

  return false;
}

CBlurayStreamCache::SourceFactory FileSourceFactory()
{
  return [](const std::string& path) -> std::unique_ptr<CBlurayStreamCache::ISource> {
    auto file = std::make_unique<CFile>();
    if (!file->Open(path, READ_NO_CACHE | READ_TRUNCATED))
      return nullptr;
    return std::make_unique<CFileSource>(std::move(file));
  };
}

CBlurayStreamCache::Config ConfigFromSettings()
{
  CBlurayStreamCache::Config config;
  config.arenaBytes = 0;

  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (!settingsComponent)
    return config;
  const auto settings = settingsComponent->GetSettings();
  if (!settings)
    return config;

  const int memoryMb = settings->GetInt(CSettings::SETTING_FILECACHE_READAHEADMEMORYSIZE);
  if (memoryMb <= 0)
    return config;

  const int behindMb = settings->GetInt(CSettings::SETTING_FILECACHE_READAHEADBEHIND);

  config.arenaBytes = static_cast<size_t>(memoryMb) * 1024 * 1024;
  config.behindBytes = static_cast<size_t>(std::max(0, behindMb)) * 1024 * 1024;

  // The settings are chosen independently, so an arena smaller than what is
  // asked to be kept behind is one click away. Clamp rather than leave no
  // forward window: half the arena behind at the very most.
  const size_t behindCeiling = config.arenaBytes / 2;
  if (config.behindBytes > behindCeiling)
  {
    CLog::Log(LOGINFO,
              "STREAMCACHE - {} MB kept behind does not fit a {} MB arena; using {} MB behind",
              behindMb, memoryMb, behindCeiling / (1024 * 1024));
    config.behindBytes = behindCeiling;
  }

  return config;
}

bool IsCacheableClip(const std::string& relPath)
{
  std::string path = relPath;
  StringUtils::Replace(path, '\\', '/');

  if (!StringUtils::EndsWithNoCase(path, ".m2ts"))
    return false;

  // BDMV/STREAM/<clip>.m2ts, and nothing one level deeper: that is
  // BDMV/STREAM/SSIF/, the 3D interleaved files, which are out of scope.
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos)
    return false;

  return StringUtils::EndsWithNoCase(path.substr(0, slash), "BDMV/STREAM");
}

std::shared_ptr<CBlurayStreamCache> ClaimFor(const std::string& path)
{
  // Every path out of here says why, at LOGINFO. Returning nullptr silently
  // made "no cache line in the log" ambiguous between the setting being off,
  // a local path, and this code never being reached - which is the first
  // thing anyone looks for when read-ahead appears not to be working.
  if (!IsRemotePath(path))
  {
    CLog::Log(LOGINFO, "STREAMCACHE - no read-ahead for {}: not a network filesystem",
              CURL::GetRedacted(path));
    return nullptr;
  }

  const CBlurayStreamCache::Config config = ConfigFromSettings();
  if (config.arenaBytes == 0)
  {
    CLog::Log(LOGINFO, "STREAMCACHE - no read-ahead for {}: read-ahead memory is set to 0",
              CURL::GetRedacted(path));
    return nullptr;
  }

  auto cache = CBlurayStreamCache::Claim(config, FileSourceFactory());
  if (!cache)
  {
    // Claim logs which of its own reasons applied (arena in use, allocation
    // refused); this says the attempt was made for this path.
    CLog::Log(LOGINFO, "STREAMCACHE - no read-ahead for {}: the arena could not be claimed",
              CURL::GetRedacted(path));
  }
  return cache;
}
} // namespace STREAMCACHE
