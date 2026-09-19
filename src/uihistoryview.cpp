// uihistoryview.cpp
//
// Copyright (c) 2019-2025 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "uihistoryview.h"

#include <set>

#include "appconfig.h"
#include "apputil.h"
#include "fileutil.h"
#include "log.h"
#include "protocolutil.h"
#include "strutil.h"
#include "timeutil.h"
#include "uicolorconfig.h"
#include "uiconfig.h"
#include "uiimagepreview.h"
#include "uimodel.h"

UiHistoryView::UiHistoryView(const UiViewParams& p_Params)
  : UiViewBase(p_Params)
{
  if (m_Enabled)
  {
    int hpad = (m_X == 0) ? 0 : 1;
    int vpad = 1;
    m_PaddedY = m_Y + vpad;
    m_PaddedX = m_X + hpad;
    m_PaddedH = m_H - (vpad * 2);
    m_PaddedW = m_W - (hpad * 2);
    m_PaddedWin = newwin(m_PaddedH, m_PaddedW, m_PaddedY, m_PaddedX);

    static int attributeTextNormal = UiColorConfig::GetAttribute("history_text_attr");
    static int colorPairTextRecv = UiColorConfig::GetColorPair("history_text_recv_color");
    werase(m_Win);
    wbkgd(m_Win, attributeTextNormal | colorPairTextRecv | ' ');
    wrefresh(m_Win);
  }
}

UiHistoryView::~UiHistoryView()
{
  if (m_PaddedWin != nullptr)
  {
    delwin(m_PaddedWin);
    m_PaddedWin = nullptr;
  }
}

void UiHistoryView::Draw()
{
  if (m_Enabled && UiImagePreview::IsEnabled() && UiImagePreview::TakeUpdated())
  {
    m_Dirty = true;
  }

  if (!m_Enabled || !m_Dirty) return;
  m_Dirty = false;

  curs_set(0);

  static int colorPairTextSent = UiColorConfig::GetColorPair("history_text_sent_color");
  static int colorPairTextRecv = UiColorConfig::GetColorPair("history_text_recv_color");
  static int colorPairTextQuoted = UiColorConfig::GetColorPair("history_text_quoted_color");
  static int colorPairTextReaction = UiColorConfig::GetColorPair("history_text_reaction_color");
  static int colorPairTextAttachment = UiColorConfig::GetColorPair("history_text_attachment_color");
  static int attributeTextNormal = UiColorConfig::GetAttribute("history_text_attr");
  static int attributeTextSelected = UiColorConfig::GetAttribute("history_text_attr_selected");

  static int colorPairNameSent = UiColorConfig::GetColorPair("history_name_sent_color");
  static int colorPairNameRecv = UiColorConfig::GetColorPair("history_name_recv_color");
  static int attributeNameNormal = UiColorConfig::GetAttribute("history_name_attr");
  static int attributeNameSelected = UiColorConfig::GetAttribute("history_name_attr_selected");

  static std::wstring attachmentIndicator =
    StrUtil::ToWString(UiConfig::GetStr("attachment_indicator") + " ");
  static std::wstring quoteIndicator = L"> ";

  std::pair<std::string, std::string>& currentChat = m_Model->GetCurrentChatLocked();
  const bool emojiEnabled = m_Model->GetEmojiEnabledLocked();
  static const bool developerMode = AppUtil::GetDeveloperMode();

  std::vector<std::string>& messageVec =
    m_Model->GetMessageVecLocked(currentChat.first, currentChat.second);
  std::unordered_map<std::string, ChatMessage>& messages =
    m_Model->GetMessagesLocked(currentChat.first, currentChat.second);
  int messageOffset = std::max(m_Model->GetMessageOffsetLocked(currentChat.first, currentChat.second), 0);

  werase(m_PaddedWin);
  wbkgd(m_PaddedWin, attributeTextNormal | colorPairTextRecv | ' ');

  m_HistoryShowCount = 0;

  // image previews are reserved as blank lines, and drawn as sixels after curses refresh
  struct PreviewPlacement
  {
    int y = 0;
    std::string path;
    bool playIcon = false;
  };
  std::vector<PreviewPlacement> previewPlacements;
  int previewMaxW = 0;
  int previewMaxH = 0;
  bool previewActive = false;
  if (UiImagePreview::IsEnabled())
  {
    int cellW = 0;
    int cellH = 0;
    if (UiImagePreview::GetCellSize(cellW, cellH))
    {
      previewMaxW = std::min(UiImagePreview::GetMaxCols(), m_PaddedW) * cellW;
      previewMaxH = ((UiImagePreview::GetRows() * cellH) / 6) * 6; // sixel bands are 6 pixels high
      previewActive = (previewMaxW > 0) && (previewMaxH > 0) && (UiImagePreview::GetRows() < (m_PaddedH - 2));
    }
  }

  bool firstMessage = true;
  int y = m_PaddedH - 1;
  for (auto it = std::next(messageVec.begin(), messageOffset); it != messageVec.end(); ++it)
  {
    bool isSelectedMessage = firstMessage && m_Model->GetSelectMessageActiveLocked();

    auto msgIt = messages.find(*it);
    if (msgIt == messages.end())
    {
      LOG_WARNING("message %s missing", it->c_str());
      continue;
    }

    ChatMessage& msg = msgIt->second;

    int attributeText = isSelectedMessage ? attributeTextSelected : attributeTextNormal;
    int colorPairText = [&]()
    {
      if (msg.isOutgoing) return colorPairTextSent;

      if (msg.senderId == currentChat.second) return colorPairTextRecv;

      static bool isUserColor = UiColorConfig::IsUserColor("history_text_recv_group_color");
      if (!isUserColor)
      {
        static int colorPairGroup = UiColorConfig::GetColorPair("history_text_recv_group_color");
        return colorPairGroup;
      }

      int colorPairGroup = UiColorConfig::GetUserColorPair("history_text_recv_group_color",
                                                           msg.senderId);
      return colorPairGroup;
    }();

    std::vector<std::wstring> wlines;
    if (!msg.text.empty())
    {
      std::string text = msg.text;
      StrUtil::SanitizeMessageStr(text);
      if (!emojiEnabled)
      {
        text = StrUtil::Textize(text);
      }

      wlines = StrUtil::WordWrap(StrUtil::ToWString(text), m_PaddedW, false, false, false, 2);
    }

    // Quoted message
    if (!msg.quotedId.empty())
    {
      std::string quotedText;
      auto quotedIt = messages.find(msg.quotedId);
      if (quotedIt != messages.end())
      {
        if (!quotedIt->second.text.empty())
        {
          quotedText = StrUtil::Split(quotedIt->second.text, '\n').at(0);
          if (!emojiEnabled)
          {
            quotedText = StrUtil::Textize(quotedText);
          }
        }
        else if (!quotedIt->second.fileInfo.empty())
        {
          FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(quotedIt->second.fileInfo);
          quotedText = FileUtil::BaseName(fileInfo.filePath);
        }
      }
      else
      {
        m_Model->FetchCachedMessageLocked(currentChat.first, currentChat.second, msg.quotedId);
        quotedText = "";
      }

      int maxQuoteLen = m_PaddedW - 3;
      std::wstring quote = quoteIndicator + StrUtil::ToWString(quotedText);
      if (StrUtil::WStringWidth(quote) > maxQuoteLen)
      {
        quote = StrUtil::TrimPadWString(quote, maxQuoteLen) + L"...";
      }

      wlines.insert(wlines.begin(), quote);
    }

    // File attachment
    std::string previewPath;
    bool previewPlayIcon = false;
    bool previewReserved = false;
    int previewRows = 0;
    const int previewStart = 1; // directly after attachment line
    if (!msg.fileInfo.empty())
    {
      FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(msg.fileInfo);

      // special case handling selection-triggered download, and handling cache's old setting
      static const bool isAttachmentPrefetchAll =
        (AppConfig::GetNum("attachment_prefetch") == AttachmentPrefetchAll);
      static const bool isAttachmentPrefetchSelected =
        (AppConfig::GetNum("attachment_prefetch") == AttachmentPrefetchSelected);
      if (isAttachmentPrefetchAll || (isSelectedMessage && isAttachmentPrefetchSelected))
      {
        if (!UiModel::IsAttachmentDownloaded(fileInfo) && UiModel::IsAttachmentDownloadable(fileInfo))
        {
          m_Model->DownloadAttachmentLocked(currentChat.first, currentChat.second, *it,
                                            fileInfo.fileId, DownloadFileActionNone);
          fileInfo = ProtocolUtil::FileInfoFromHex(msg.fileInfo);
        }
      }

      std::string fileName = FileUtil::BaseName(fileInfo.filePath);
      std::string fileStatus;
      if (fileInfo.fileStatus == FileStatusNone)
      {
        // should not happen
        static const std::string statusNone = " -";
        fileStatus = statusNone;
      }
      else if (fileInfo.fileStatus == FileStatusNotDownloaded)
      {
        static const std::string statusNotDownloaded = " " + UiConfig::GetStr("downloadable_indicator");
        fileStatus = statusNotDownloaded;
      }
      else if (fileInfo.fileStatus == FileStatusDownloaded)
      {
        static const std::string statusDownloaded = "";
        fileStatus = statusDownloaded;
      }
      else if (fileInfo.fileStatus == FileStatusDownloading)
      {
        static const std::string statusDownloading = " " + UiConfig::GetStr("syncing_indicator");
        fileStatus = statusDownloading;
      }
      else if (fileInfo.fileStatus == FileStatusDownloadFailed)
      {
        static const std::string statusDownloadFailed = " " + UiConfig::GetStr("failed_indicator");
        fileStatus = statusDownloadFailed;
      }

      std::wstring fileStr = attachmentIndicator + StrUtil::ToWString(fileName + fileStatus);
      wlines.insert(wlines.begin(), fileStr);

      if (previewActive)
      {
        if (UiModel::IsAttachmentDownloaded(fileInfo) && UiImagePreview::IsPreviewable(fileInfo.filePath))
        {
          previewPath = fileInfo.filePath;
        }
        else if ((fileInfo.filePath == "[Photo]") && UiModel::IsAttachmentDownloadable(fileInfo))
        {
          // reserve space while downloading photo to keep layout stable
          m_Model->DownloadAttachmentLocked(currentChat.first, currentChat.second, *it,
                                            fileInfo.fileId, DownloadFileActionNone);
          previewReserved = true;
        }
        else if ((fileInfo.filePath == "[Photo]") && (fileInfo.fileStatus == FileStatusDownloading))
        {
          previewReserved = true;
        }
        else if ((fileInfo.filePath == "[Sticker]") && UiModel::IsAttachmentDownloadable(fileInfo))
        {
          // stickers may be animated (not previewable), so reserve space only once downloaded
          m_Model->DownloadAttachmentLocked(currentChat.first, currentChat.second, *it,
                                            fileInfo.fileId, DownloadFileActionNone);
        }

        static const std::set<std::string> videoPlaceholders = { "[Video]", "[VideoNote]", "[Animation]" };
        static const std::set<std::string> videoExts = { ".mp4", ".mov", ".webm", ".mkv", ".m4v", ".gif" };
        const bool isVideo = (videoPlaceholders.count(fileInfo.filePath) > 0) ||
          (videoExts.count(StrUtil::ToLower(FileUtil::GetFileExt(fileInfo.filePath))) > 0);

        // messages cached before thumbnail support need to be refreshed to get thumbnail info
        if (previewPath.empty() && !previewReserved && fileInfo.thumbId.empty() && fileInfo.thumbPath.empty() &&
            isVideo)
        {
          m_Model->RefreshMessageLocked(currentChat.first, currentChat.second, *it);
        }

        // fall back to thumbnail, e.g. first frame of video
        if (previewPath.empty() && !previewReserved && !fileInfo.thumbId.empty())
        {
          if (!fileInfo.thumbPath.empty() && UiImagePreview::IsPreviewable(fileInfo.thumbPath))
          {
            previewPath = fileInfo.thumbPath;
            previewPlayIcon = isVideo;
          }
          else if (fileInfo.thumbPath.empty())
          {
            if (UiImagePreview::MarkRequested(fileInfo.thumbId))
            {
              m_Model->DownloadAttachmentLocked(currentChat.first, currentChat.second, *it,
                                                fileInfo.thumbId, DownloadFileActionThumb);
            }

            previewReserved = true;
          }
        }

        // skip images which could not be converted
        if (!previewPath.empty() && UiImagePreview::IsFailed(previewPath, previewMaxW, previewMaxH, previewPlayIcon))
        {
          previewPath.clear();
        }

        if (!previewPath.empty() || previewReserved)
        {
          wlines.insert(wlines.begin() + 1, UiImagePreview::GetRows(), std::wstring());
          previewRows = UiImagePreview::GetRows();
        }
      }
    }

    // Reactions
    int reactionLines = 0;
    static bool reactionsEnabled = UiConfig::GetBool("reactions_enabled");
    if (reactionsEnabled)
    {
      std::string selfEmoji;
      auto sit = msg.reactions.senderEmojis.find(s_ReactionsSelfId);
      if (sit != msg.reactions.senderEmojis.end())
      {
        selfEmoji = sit->second;
      }

      // Allow also if we have self emoji, even if not yet consolidated into count
      if (!msg.reactions.emojiCounts.empty() || !selfEmoji.empty())
      {
        bool foundSelf = false;
        std::string reactionsText;
        std::multimap<float, std::string> emojiCountsSorted;
        for (const auto& emojiCount : msg.reactions.emojiCounts)
        {
          float count = emojiCount.second;
          if (emojiCount.first == selfEmoji)
          {
            count += 0.1; // for equal count, prioritize own selected reaction
            foundSelf = true;
          }

          emojiCountsSorted.insert(std::make_pair(count, emojiCount.first));
        }

        if (!foundSelf && !selfEmoji.empty())
        {
          LOG_DEBUG("insert missing reaction for self");
          emojiCountsSorted.insert(std::make_pair(1.1, selfEmoji));
        }

        bool firstReaction = true;
        for (auto emojiCount = emojiCountsSorted.rbegin(); emojiCount != emojiCountsSorted.rend(); ++emojiCount)
        {
          reactionsText += (firstReaction ? " " : "  ");
          if (emojiCount->second == selfEmoji)
          {
            // Highlight own reaction emoji
            reactionsText += "" + emojiCount->second + "*";
          }
          else
          {
            reactionsText += emojiCount->second;
          }

          if (emojiCount->first > 1.5)
          {
            reactionsText += " " + FileUtil::GetSuffixedCount(static_cast<ssize_t>(emojiCount->first));
          }

          firstReaction = false;
        }

        if (!reactionsText.empty())
        {
          if (!emojiEnabled)
          {
            reactionsText = StrUtil::Textize(reactionsText);
          }

          const int maxReactionsLen = m_PaddedW - 4;
          std::wstring reactions = StrUtil::ToWString(reactionsText);
          if (StrUtil::WStringWidth(reactions) > maxReactionsLen)
          {
            reactions = StrUtil::TrimPadWString(reactions, maxReactionsLen) + L"... ";
          }
          else
          {
            reactions += L" ";
          }

          wlines.insert(wlines.end(), reactions);
          reactionLines = 1;
        }
      }
    }

    const int maxMessageLines = (m_PaddedH - 1);
    if (firstMessage && ((int)wlines.size() > maxMessageLines))
    {
      wlines.resize(maxMessageLines - 1);
      wlines.push_back(L"[...]");
      reactionLines = 0;
      if ((previewStart + previewRows) > (maxMessageLines - 1))
      {
        previewPath.clear();
      }
    }

    for (auto wline = wlines.rbegin(); wline != wlines.rend(); ++wline)
    {
      if (!previewPath.empty() && (std::distance(wline, wlines.rend()) - 1 == previewStart))
      {
        // top line of preview reached, i.e. whole preview is visible
        previewPlacements.push_back(PreviewPlacement{ y, previewPath, previewPlayIcon });
      }

      bool isAttachment = (wline->rfind(attachmentIndicator, 0) == 0);
      bool isQuote = (wline->rfind(quoteIndicator, 0) == 0);
      bool isReaction = (reactionLines == 1) && (std::distance(wline, wlines.rbegin()) == 0);

      if (isAttachment)
      {
        wattron(m_PaddedWin, attributeText | colorPairTextAttachment);
      }
      else if (isQuote)
      {
        wattron(m_PaddedWin, attributeText | colorPairTextQuoted);
      }
      else if (isReaction)
      {
        wattron(m_PaddedWin, attributeTextNormal | colorPairTextReaction);
      }
      else
      {
        wattron(m_PaddedWin, attributeText | colorPairText);
      }

      const std::wstring wdisp = isReaction ? *wline : StrUtil::TrimPadWString(*wline, m_PaddedW);
      mvwaddnwstr(m_PaddedWin, y, 0, wdisp.c_str(), std::min((int)wdisp.size(), m_PaddedW));

      if (isAttachment)
      {
        wattroff(m_PaddedWin, attributeText | colorPairTextAttachment);
      }
      else if (isQuote)
      {
        wattroff(m_PaddedWin, attributeText | colorPairTextQuoted);
      }
      else if (isReaction)
      {
        wattroff(m_PaddedWin, attributeTextNormal | colorPairTextReaction);
      }
      else
      {
        wattroff(m_PaddedWin, attributeText | colorPairText);
      }

      if (--y < 0) break;
    }

    if (y < 0) break;

    int attributeName = isSelectedMessage ? attributeNameSelected : attributeNameNormal;
    int colorPairName = [&]()
    {
      if (msg.isOutgoing) return colorPairNameSent;

      if (msg.senderId == currentChat.second) return colorPairNameRecv;

      static bool isUserColor = UiColorConfig::IsUserColor("history_name_recv_group_color");
      if (!isUserColor)
      {
        static int colorPairGroup = UiColorConfig::GetColorPair("history_name_recv_group_color");
        return colorPairGroup;
      }

      int colorPairGroup = UiColorConfig::GetUserColorPair("history_name_recv_group_color",
                                                           msg.senderId);
      return colorPairGroup;
    }();

    wattron(m_PaddedWin, attributeName | colorPairName);
    std::string name = m_Model->GetContactNameLocked(currentChat.first, msg.senderId);
    if (!emojiEnabled)
    {
      name = StrUtil::Textize(name);
    }

    std::wstring wsender = StrUtil::ToWString(name);
    std::wstring wtime;
    if (developerMode)
    {
      wtime = L" (" + StrUtil::ToWString(std::to_string(msg.timeSent)) + L")";
    }
    else
    {
      if (msg.timeSent != std::numeric_limits<int64_t>::max())
      {
        wtime = L" (" + StrUtil::ToWString(TimeUtil::GetTimeString(msg.timeSent, false /* p_IsExport */)) + L")";
      }
    }

    m_Model->MarkReadLocked(currentChat.first, currentChat.second, *it, (!msg.isOutgoing && !msg.isRead));

    static const std::string readIndicator = " " + UiConfig::GetStr("read_indicator");
    static const std::string editedIndicator = " " + UiConfig::GetStr("edited_indicator");
    static const std::string pinnedIndicator = " " + UiConfig::GetStr("pinned_indicator");
    std::wstring wreceipt = StrUtil::ToWString(msg.isRead ? readIndicator : "");
    std::wstring wedited = StrUtil::ToWString(msg.isEdited ? editedIndicator : "");
    std::wstring wpinned = StrUtil::ToWString(msg.isPinned ? pinnedIndicator : "");
    std::wstring wheader = wsender + wtime + wreceipt + wedited + wpinned;

    if (developerMode)
    {
      wheader = wheader +
        L" msg " + StrUtil::ToWString(msg.id) +
        L" user " + StrUtil::ToWString(msg.senderId);
    }

    std::wstring wdisp = StrUtil::TrimPadWString(wheader, m_PaddedW);
    mvwaddnwstr(m_PaddedWin, y, 0, wdisp.c_str(), std::min((int)wdisp.size(), m_PaddedW));

    wattroff(m_PaddedWin, attributeName | colorPairName);

    ++m_HistoryShowCount;

    if (--y < 0) break;

    if (--y < 0) break;

    firstMessage = false;
  }

  // repaint all cells to clear any previously drawn sixels
  static bool hadPreviews = false;
  if (previewActive && (hadPreviews || !previewPlacements.empty()))
  {
    redrawwin(m_PaddedWin);
  }

  hadPreviews = previewActive && !previewPlacements.empty();

  wrefresh(m_PaddedWin);

  if (UiImagePreview::IsSuppressed()) return;

  for (const auto& placement : previewPlacements)
  {
    std::shared_ptr<const std::string> sixel =
      UiImagePreview::GetSixel(placement.path, previewMaxW, previewMaxH, placement.playIcon);
    if (sixel)
    {
      UiImagePreview::Output(*sixel, m_PaddedY + placement.y, m_PaddedX);
    }
  }
}

int UiHistoryView::GetHistoryShowCount()
{
  return m_HistoryShowCount;
}
