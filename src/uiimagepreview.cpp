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
#include <set>
#include <thread>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
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
    bool playIcon = false;
    std::string label;
  };

  std::mutex s_Mutex;
  std::condition_variable s_CondVar;
  std::map<std::string, Entry> s_Cache;
  std::set<std::string> s_Requested;
  std::deque<Job> s_Jobs;
  std::thread s_Thread;
  bool s_Running = false;
  std::atomic<bool> s_Updated(false);
  int s_SuppressCount = 0;
  bool s_Enabled = false;

  const size_t s_MaxCacheEntries = 200;

  std::string Encode(const Job& p_Job)
  {
    static const std::string commandTemplate = []()
    {
      std::string command = UiConfig::GetStr("attachment_preview_command");
      if (command.empty())
      {
        command = "magick '%1[0]' -auto-orient -resize %2x%3 %4 -colors 255 sixel:- 2>/dev/null";
      }

      return command;
    }();

    std::string command = commandTemplate;
    StrUtil::ReplaceString(command, "%1", StrUtil::EscapeSingleQuote(p_Job.path));
    StrUtil::ReplaceString(command, "%2", std::to_string(p_Job.maxW));
    StrUtil::ReplaceString(command, "%3", std::to_string(p_Job.maxH));

    std::string playIconArgs;
    if (p_Job.playIcon)
    {
      // translucent circle with triangle composited at center
      const int size = std::max(28, std::min(64, std::min(p_Job.maxW, p_Job.maxH) / 4));
      const int c = size / 2;
      const int r = c - 1;
      const int t = size * 3 / 8;
      auto pt = [](int x, int y) { return std::to_string(x) + "," + std::to_string(y); };
      playIconArgs = "'(' -size " + std::to_string(size) + "x" + std::to_string(size) + " xc:none"
        " -fill '#000000a0' -draw 'circle " + pt(c, c) + " " + pt(c, c - r) + "'"
        " -fill white -draw 'polygon " + pt(c - t / 2, c - t / 2 - 2) + " " + pt(c - t / 2, c + t / 2 + 2) + " " +
        pt(c + t / 2 + 2, c) + "' ')' -gravity center -composite";
    }

    if (!p_Job.label.empty())
    {
      // label with translucent background at top left, label is only digits and colons
      const int pointSize = std::max(11, std::min(20, p_Job.maxH / 15));
      playIconArgs += " -font \"$(fc-match -f '%{file}' sans:bold 2>/dev/null)\" -gravity northwest"
        " -fill white -undercolor '#00000090' -pointsize " + std::to_string(pointSize) +
        " -annotate +5+5 ' " + p_Job.label + " '";
    }

    StrUtil::ReplaceString(command, "%4", playIconArgs);

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

static bool DetectSixelSupport()
{
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

  struct termios oldTio;
  if (tcgetattr(STDIN_FILENO, &oldTio) != 0) return false;

  struct termios newTio = oldTio;
  newTio.c_lflag &= ~(ICANON | ECHO);
  newTio.c_cc[VMIN] = 0;
  newTio.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &newTio);

  // primary device attributes, response is ESC [ ? Ps ; ... c where Ps 4 means sixel
  const std::string query = "\033[c";
  fflush(stdout);
  bool rv = (write(STDOUT_FILENO, query.data(), query.size()) == (ssize_t)query.size());

  std::string response;
  const int timeoutMs = 500;
  while (rv)
  {
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&pfd, 1, timeoutMs) <= 0) break;

    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) break;

    response += c;
    if ((c == 'c') && (response.find("\033[?") != std::string::npos)) break;
  }

  tcsetattr(STDIN_FILENO, TCSANOW, &oldTio);

  const std::string attrs = StrUtil::ExtractString(response, "\033[?", "c");
  const std::vector<std::string> params = StrUtil::Split(attrs, ';');
  const bool hasSixel = (std::find(params.begin(), params.end(), "4") != params.end());
  LOG_DEBUG("terminal sixel support %d", hasSixel);
  return hasSixel;
}

void UiImagePreview::Init()
{
  // 0 = disabled, 1 = enabled if terminal supports sixel, 2 = always enabled
  const int mode = UiConfig::GetNum("attachment_preview_enabled");
  s_Enabled = (mode == 2) || ((mode == 1) && DetectSixelSupport());
}

bool UiImagePreview::IsEnabled()
{
  return s_Enabled;
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

static std::string GetKey(const std::string& p_Path, int p_MaxW, int p_MaxH, bool p_PlayIcon,
                          const std::string& p_Label)
{
  return p_Path + "|" + std::to_string(p_MaxW) + "x" + std::to_string(p_MaxH) + (p_PlayIcon ? "|play" : "") +
    "|" + p_Label;
}

bool UiImagePreview::IsFailed(const std::string& p_Path, int p_MaxW, int p_MaxH, bool p_PlayIcon,
                              const std::string& p_Label)
{
  std::unique_lock<std::mutex> lock(s_Mutex);
  auto it = s_Cache.find(GetKey(p_Path, p_MaxW, p_MaxH, p_PlayIcon, p_Label));
  return (it != s_Cache.end()) && (it->second.state == State::Failed);
}

bool UiImagePreview::MarkRequested(const std::string& p_Id)
{
  return s_Requested.insert(p_Id).second;
}

std::shared_ptr<const std::string> UiImagePreview::GetSixel(const std::string& p_Path, int p_MaxW, int p_MaxH,
                                                            bool p_PlayIcon, const std::string& p_Label)
{
  const std::string key = GetKey(p_Path, p_MaxW, p_MaxH, p_PlayIcon, p_Label);

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
  s_Jobs.push_back(Job{ key, p_Path, p_MaxW, p_MaxH, p_PlayIcon, p_Label });

  if (!s_Running)
  {
    s_Running = true;
    s_Thread = std::thread(Worker);
  }

  s_CondVar.notify_one();
  return nullptr;
}

std::string UiImagePreview::FormatDuration(int p_Seconds)
{
  char buf[32];
  const int h = p_Seconds / 3600;
  const int m = (p_Seconds % 3600) / 60;
  const int s = p_Seconds % 60;
  if (h > 0)
  {
    snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
  }
  else
  {
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
  }

  return buf;
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
