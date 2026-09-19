// uihistoryview.h
//
// Copyright (c) 2019-2023 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <set>
#include <string>
#include <vector>

#include "uiviewbase.h"

class UiHistoryView : public UiViewBase
{
public:
  UiHistoryView(const UiViewParams& p_Params);
  virtual ~UiHistoryView();

  virtual void Draw();
  int GetHistoryShowCount();

private:
  struct PreviewPlacement
  {
    int y = 0;
    std::string path;
    bool playIcon = false;
    std::string label;

    bool operator==(const PreviewPlacement& p_Other) const
    {
      return (y == p_Other.y) && (path == p_Other.path) && (playIcon == p_Other.playIcon) &&
             (label == p_Other.label);
    }
  };

  std::string GetTimeString(int64_t p_TimeSent);
  std::vector<size_t> GetRowHashes();

private:
  WINDOW* m_PaddedWin = nullptr;
  int m_PaddedY = 0;
  int m_PaddedX = 0;
  int m_PaddedH = 0;
  int m_PaddedW = 0;
  int m_HistoryShowCount = 0;

  // previously drawn previews, to only redraw sixels when needed
  bool m_PrevValid = false;
  std::vector<PreviewPlacement> m_PrevPlacements;
  std::vector<size_t> m_PrevRowHashes;
  std::set<size_t> m_PrevShown;
};
