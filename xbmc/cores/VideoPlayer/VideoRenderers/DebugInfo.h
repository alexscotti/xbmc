/*
 *  Copyright (C) 2005-2021 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

struct DEBUG_INFO_PLAYER
{
  std::string audio;
  std::string video;
  std::string player;
  std::string vsync;
  // Read-ahead cache state, empty when this stream has no cache. Its own row:
  // the renderer emits one subtitle line per field, so a newline embedded in
  // another field is swallowed rather than making a new line.
  std::string readahead;
};

struct DEBUG_INFO_VIDEO
{
  std::string videoSource;
  std::string metaPrim;
  std::string metaLight;
  std::string shader;
  std::string render;
};

struct DEBUG_INFO_RENDER
{
  std::string renderFlags;
  std::string videoOutput;
};
