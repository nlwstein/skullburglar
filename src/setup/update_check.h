// New-version check for the launcher. Release builds carry their version and the address of a small JSON file
// listing the releases (BOTS_VERSION, BOTS_UPDATE_URL: CMake cache variables the release CI sets); other builds have
// neither and never check. The launcher fetches the file in the background when it opens (unless the check_updates
// setting is off) and, when a newer version is out, offers to open its download page in the browser.
//
// The file is JSON; the check reads two top-level strings from it:
//   {"latest": "0.4.0", "page": "https://.../", ...}
#pragma once

#include <string>

namespace bots::setup {

// This build's version ("0.4.0"), or "dev".
const char* AppVersion();

// True when this build knows where to look (a release build).
bool UpdateCheckAvailable();

struct UpdateInfo {
  std::string latest;  // newest released version, empty when the check failed
  std::string page;    // its download page
  std::string error;   // why the check failed, for the log
};

// Fetches and reads the releases file. Blocks for up to a few seconds; run it off the UI thread.
UpdateInfo FetchUpdateInfo();

// True when version `a` is newer than `b` (dotted numbers; a suffix such as "-dev.abc" is ignored).
bool IsNewerVersion(const std::string& a, const std::string& b);

// Reads a top-level "key": "value" string from a JSON document; empty when missing.
std::string JsonStringField(const std::string& json, const std::string& key);

// Opens an http(s) address in the default browser.
void OpenInBrowser(const std::string& url);

}  // namespace bots::setup
