// New-version check for the launcher. Release builds carry their version and the address of a small JSON file
// listing the releases (BOTS_VERSION, BOTS_UPDATE_URL: CMake cache variables the release CI sets); other builds have
// neither and never check. The launcher fetches the file in the background when it opens (unless the check_updates
// setting is off) and, when a newer version is out, offers to open its download page in the browser.
//
// The file is JSON (update_logic.h reads it): "latest" and "page" at the top, and per version the packages with their
// size and SHA-256, which "Update now" (update_install.h) downloads, and an Ed25519 signature over them that must
// verify against the key compiled into this build:
//   {"latest": "0.4.0", "page": "https://.../", "versions": [{"version": "0.4.0", "signature": "<128 hex>", "downloads": {...}}]}
#pragma once

#include <string>

#include "update_logic.h"

namespace bots::setup {

// This build's version ("0.4.0"), or "dev".
const char* AppVersion();

// True when this build knows where to look (a release build; the address must be https).
bool UpdateCheckAvailable();

// True when this build carries the update signing key (BOTS_UPDATE_PUBKEY, packaging/update-pubkey.txt). Without it no
// release counts as signed, so "Update now" never shows and only "Download it" does.
bool SelfUpdateKeyConfigured();

// Fetches and reads the releases file. Blocks for up to a few seconds; run it off the UI thread.
UpdateInfo FetchUpdateInfo();

// Opens an http(s) address in the default browser.
void OpenInBrowser(const std::string& url);

}  // namespace bots::setup
