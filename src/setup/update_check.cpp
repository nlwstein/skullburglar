// See update_check.h.
#include "update_check.h"

#include "ed25519.h"

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
#ifndef BOTS_UPDATE_PUBKEY
#define BOTS_UPDATE_PUBKEY ""
#endif

namespace bots::setup {

namespace {

constexpr const char* kUpdateUrl = BOTS_UPDATE_URL;
constexpr const char* kPublicKey = BOTS_UPDATE_PUBKEY;
#ifdef BOTS_ALLOW_HTTP_UPDATE_URL
constexpr bool kAllowHttp = true;  // a local test server only; CMake's BOTS_ALLOW_HTTP_UPDATE_URL, off in releases
#else
constexpr bool kAllowHttp = false;
#endif
constexpr int kTimeoutMs = 5000;
constexpr size_t kMaxBody = 1 << 20;

bool IsHttpUrl(const std::string& url) { return url.starts_with("https://") || url.starts_with("http://"); }

// The address the releases file is read from: https, or http when the build allows it (tests).
bool IsUpdateUrl(const std::string& url) { return url.starts_with("https://") || (kAllowHttp && url.starts_with("http://")); }

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
#if defined(__APPLE__)
  const char* curl = "/usr/bin/curl";
#else
  const char* curl = "curl";
#endif
  const char* proto = url.starts_with("https://") ? "=https" : "=http";
  std::string cmd = std::string(curl) + " -fsSL --proto " + proto + " --proto-redir " + proto + " --max-filesize " +
                    std::to_string(kMaxBody) + " --max-time " + std::to_string(kTimeoutMs / 1000) + " '" + url +
                    "' 2>/dev/null";
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

}  // namespace

const char* AppVersion() { return BOTS_VERSION; }

bool UpdateCheckAvailable() { return IsUpdateUrl(kUpdateUrl) && HasVersionNumber(BOTS_VERSION); }

bool SelfUpdateKeyConfigured() {
  uint8_t key[32];
  return HexToBytes(kPublicKey, key, 32);
}

UpdateInfo FetchUpdateInfo() {
  std::string error;
  std::string body = Fetch(kUpdateUrl, error);
  if (!error.empty()) {
    UpdateInfo info;
    info.error = error;
    return info;
  }
  return ParseReleases(body, kUpdateUrl, SelfUpdateKeyConfigured() ? kPublicKey : "");
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
