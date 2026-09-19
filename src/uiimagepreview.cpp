// uiimagepreview.cpp
//
// Copyright (c) 2026 Shinrai Hikaro
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "uiimagepreview.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <sys/ioctl.h>
#include <unistd.h>

#include "fileutil.h"
#include "log.h"
#include "strutil.h"
#include "uiconfig.h"

namespace
{
  enum class State
  {
    Pending,
    Ready,
    Failed,
  };

  struct Entry
  {
    State state = State::Pending;
    std::shared_ptr<const std::string> sixel;
  };

  struct Job
  {
    std::string key;
    std::string path;
    int maxW = 0;
    int maxH = 0;
  };

  std::mutex s_Mutex;
  std::condition_variable s_CondVar;
  std::map<std::string, Entry> s_Cache;
  std::deque<Job> s_Jobs;
  std::thread s_Thread;
  bool s_Running = false;
  std::atomic<bool> s_Updated(false);
  int s_SuppressCount = 0;

  const size_t s_MaxCacheEntries = 200;

  std::string Encode(const Job& p_Job)
  {
    static const std::string commandTemplate = []()
    {
      std::string command = UiConfig::GetStr("attachment_preview_command");
      if (command.empty())
      {
        command = "magick '%1[0]' -auto-orient -resize %2x%3 -colors 255 sixel:- 2>/dev/null";
      }

      return command;
    }();

    std::string command = commandTemplate;
    StrUtil::ReplaceString(command, "%1", StrUtil::EscapeSingleQuote(p_Job.path));
    StrUtil::ReplaceString(command, "%2", std::to_string(p_Job.maxW));
    StrUtil::ReplaceString(command, "%3", std::to_string(p_Job.maxH));

    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return output;

    char buf[65536];
    size_t len = 0;
    while ((len = fread(buf, 1, sizeof(buf), pipe)) > 0)
    {
      output.append(buf, len);
    }

    int rv = pclose(pipe);
    if ((rv != 0) || (output.rfind("\033P", 0) != 0))
    {
      LOG_WARNING("preview encoding failed (%d) for %s", rv, p_Job.path.c_str());
      output.clear();
    }

    return output;
  }

  void Worker()
  {
    std::unique_lock<std::mutex> lock(s_Mutex);
    while (true)
    {
      s_CondVar.wait(lock, []() { return !s_Running || !s_Jobs.empty(); });
      if (!s_Running) break;

      Job job = s_Jobs.front();
      s_Jobs.pop_front();

      lock.unlock();
      std::string sixel = Encode(job);
      lock.lock();

      Entry& entry = s_Cache[job.key];
      if (sixel.empty())
      {
        entry.state = State::Failed;
      }
      else
      {
        entry.state = State::Ready;
        entry.sixel = std::make_shared<const std::string>(std::move(sixel));
      }

      s_Updated = true;
    }
  }
}

bool UiImagePreview::IsEnabled()
{
  static const bool enabled = UiConfig::GetBool("attachment_preview_enabled");
  return enabled;
}

int UiImagePreview::GetRows()
{
  static const int rows = std::max(2, UiConfig::GetNum("attachment_preview_rows"));
  return rows;
}

int UiImagePreview::GetMaxCols()
{
  static const int cols = std::max(4, UiConfig::GetNum("attachment_preview_max_cols"));
  return cols;
}

bool UiImagePreview::GetCellSize(int& p_W, int& p_H)
{
  struct winsize ws = { };
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) ||
      (ws.ws_col == 0) || (ws.ws_row == 0) || (ws.ws_xpixel == 0) || (ws.ws_ypixel == 0))
  {
    return false;
  }

  p_W = ws.ws_xpixel / ws.ws_col;
  p_H = ws.ws_ypixel / ws.ws_row;
  return (p_W > 0) && (p_H > 0);
}

bool UiImagePreview::IsPreviewable(const std::string& p_Path)
{
  static std::map<std::string, bool> previewable;
  auto it = previewable.find(p_Path);
  if (it != previewable.end()) return it->second;

  bool rv = false;
  if (FileUtil::Exists(p_Path) && !FileUtil::IsDir(p_Path))
  {
    const std::string mime = FileUtil::GetMimeType(p_Path);
    rv = (mime.rfind("image/", 0) == 0) && (mime != "image/svg+xml");
  }

  previewable[p_Path] = rv;
  return rv;
}

std::shared_ptr<const std::string> UiImagePreview::GetSixel(const std::string& p_Path, int p_MaxW, int p_MaxH)
{
  const std::string key = p_Path + "|" + std::to_string(p_MaxW) + "x" + std::to_string(p_MaxH);

  std::unique_lock<std::mutex> lock(s_Mutex);
  auto it = s_Cache.find(key);
  if (it != s_Cache.end())
  {
    return (it->second.state == State::Ready) ? it->second.sixel : nullptr;
  }

  if (s_Cache.size() >= s_MaxCacheEntries)
  {
    // simple eviction, keep only pending entries
    for (auto cit = s_Cache.begin(); cit != s_Cache.end();)
    {
      cit = (cit->second.state == State::Pending) ? std::next(cit) : s_Cache.erase(cit);
    }
  }

  s_Cache[key] = Entry();
  s_Jobs.push_back(Job{ key, p_Path, p_MaxW, p_MaxH });

  if (!s_Running)
  {
    s_Running = true;
    s_Thread = std::thread(Worker);
  }

  s_CondVar.notify_one();
  return nullptr;
}

bool UiImagePreview::TakeUpdated()
{
  return s_Updated.exchange(false);
}

void UiImagePreview::Output(const std::string& p_Sixel, int p_Y, int p_X)
{
  // save cursor, position, draw, restore cursor
  std::string out = "\0337\033[" + std::to_string(p_Y + 1) + ";" + std::to_string(p_X + 1) + "H";
  out += p_Sixel;
  out += "\0338";

  fflush(stdout);
  size_t pos = 0;
  while (pos < out.size())
  {
    ssize_t rv = write(STDOUT_FILENO, out.data() + pos, out.size() - pos);
    if (rv <= 0) break;

    pos += rv;
  }
}

void UiImagePreview::Suppress(bool p_Suppress)
{
  s_SuppressCount = std::max(0, s_SuppressCount + (p_Suppress ? 1 : -1));
}

bool UiImagePreview::IsSuppressed()
{
  return (s_SuppressCount > 0);
}

void UiImagePreview::Cleanup()
{
  {
    std::unique_lock<std::mutex> lock(s_Mutex);
    if (!s_Running) return;

    s_Running = false;
    s_Jobs.clear();
  }

  s_CondVar.notify_one();
  if (s_Thread.joinable())
  {
    s_Thread.join();
  }
}
