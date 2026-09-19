// uiimagepreview.h
//
// Copyright (c) 2026 Shinrai Hikaro
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <memory>
#include <string>

// Inline image previews rendered as sixel graphics. Encoding is done by an
// external command (ImageMagick) in a background thread, results are cached.
class UiImagePreview
{
public:
  static bool IsEnabled();
  static int GetRows();
  static int GetMaxCols();

  // terminal cell size in pixels, false if unknown
  static bool GetCellSize(int& p_W, int& p_H);

  // true if file is a (still) image that can be previewed
  static bool IsPreviewable(const std::string& p_Path);

  // returns sixel data, or nullptr if not yet encoded (encoding is queued) or failed
  static std::shared_ptr<const std::string> GetSixel(const std::string& p_Path, int p_MaxW, int p_MaxH);

  // true once after a queued encoding completed, i.e. a redraw is needed
  static bool TakeUpdated();

  // write sixel at given 0-based screen position, bypassing curses
  static void Output(const std::string& p_Sixel, int p_Y, int p_X);

  // suppress output while modal dialogs are shown (nestable)
  static void Suppress(bool p_Suppress);
  static bool IsSuppressed();

  static void Cleanup();
};
