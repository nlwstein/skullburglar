// See update_check.h.
#include "update_check.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#ifndef BOTS_VERSION
#define BOTS_VERSION "dev"
#endif
#ifndef BOTS_UPDATE_URL
#define BOTS_UPDATE_URL ""
#endif

namespace bots::setup {

namespace {

constexpr const char* kUpdateUrl = BOTS_UPDATE_URL;
constexpr int kTimeoutMs = 5000;
constexpr size_t kMaxBody = 1 << 20;

bool IsHttpUrl(const std::string& url) { return url.starts_with("https://") || url.starts_with("http://"); }

#if defined(_WIN32)
std::wstring Widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Fetch(const std::string& url, std::string& error) {
  std::wstring wurl = Widen(url);
  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256], path[2048];
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
    error = "bad address";
    return {};
  }
  std::string body;
  HINTERNET session = WinHttpOpen(L"Skullburglar", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
  DWORD status = 0, size = sizeof(status);
  if (request) WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) ||
      !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                           &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    error = "request failed (error " + std::to_string(GetLastError()) + ")";
  } else if (status != 200) {
    error = "HTTP " + std::to_string(status);
  } else {
    char buf[8192];
    DWORD got = 0;
    while (WinHttpReadData(request, buf, sizeof(buf), &got) && got && body.size() < kMaxBody) body.append(buf, got);
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
  return body;
}
#else
// curl ships with macOS and practically every Linux, SteamOS included.
std::string Fetch(const std::string& url, std::string& error) {
  if (url.find('\'') != std::string::npos) {
    error = "bad address";
    return {};
  }
  std::string cmd = "curl -fsSL --max-time " + std::to_string(kTimeoutMs / 1000) + " '" + url + "' 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    error = "can't run curl";
    return {};
  }
  std::string body;
  char buf[8192];
  for (size_t got; (got = std::fread(buf, 1, sizeof(buf), pipe)) > 0 && body.size() < kMaxBody;) body.append(buf, got);
  int status = pclose(pipe);
  if (status != 0) {
    error = "curl failed (exit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")";
    return {};
  }
  return body;
}
#endif

std::vector<long> VersionNumbers(const std::string& v) {
  std::vector<long> out;
  const char* p = v.c_str();
  if (*p == 'v') ++p;
  while (*p >= '0' && *p <= '9') {
    char* end;
    out.push_back(std::strtol(p, &end, 10));
    p = end;
    if (*p != '.') break;
    ++p;
  }
  return out;
}

}  // namespace

const char* AppVersion() { return BOTS_VERSION; }

bool UpdateCheckAvailable() { return IsHttpUrl(kUpdateUrl) && !VersionNumbers(BOTS_VERSION).empty(); }

UpdateInfo FetchUpdateInfo() {
  UpdateInfo info;
  std::string body = Fetch(kUpdateUrl, info.error);
  if (!info.error.empty()) return info;
  info.latest = JsonStringField(body, "latest");
  info.page = JsonStringField(body, "page");
  if (VersionNumbers(info.latest).empty()) {
    info.latest.clear();
    info.error = "no version in the reply";
  } else if (!IsHttpUrl(info.page)) {
    info.page.clear();
  }
  return info;
}

bool IsNewerVersion(const std::string& a, const std::string& b) {
  auto x = VersionNumbers(a), y = VersionNumbers(b);
  x.resize(std::max(x.size(), y.size()));
  y.resize(x.size());
  return x > y;
}

std::string JsonStringField(const std::string& json, const std::string& key) {
  int depth = 0;
  std::string last;  // the most recent string at depth 1
  bool after_key = false;
  for (size_t i = 0; i < json.size(); ++i) {
    char c = json[i];
    if (c == '"') {
      std::string s;
      for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;  // \" \\ \/ as the character; others are not expected
        s += json[i];
      }
      if (depth != 1) continue;
      if (after_key) {
        if (last == key) return s;
        after_key = false;
      } else {
        last = s;
      }
    } else if (c == ':') {
      after_key = depth == 1;
    } else if (c == ',') {
      after_key = false;
    } else if (c == '{' || c == '[') {
      ++depth;
      after_key = false;
    } else if (c == '}' || c == ']') {
      --depth;
    }
  }
  return {};
}

void OpenInBrowser(const std::string& url) {
  if (!IsHttpUrl(url)) return;
#if defined(_WIN32)
  ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
  const char* opener = "open";
#else
  const char* opener = "xdg-open";
#endif
  char* argv[] = {const_cast<char*>(opener), const_cast<char*>(url.c_str()), nullptr};
  pid_t pid;
  if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) == 0) {
    // xdg-open can stay around for a while; don't wait on it here.
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
  }
#endif
}

}  // namespace bots::setup
