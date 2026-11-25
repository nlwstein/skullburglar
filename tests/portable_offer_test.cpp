// Plain-assert tests for src/setup/portable_offer.h (B-408): when the launcher offers portable mode, and the answer files.
// Run: c++ -std=c++20 -Wall -Werror -I src tests/portable_offer_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "setup/portable_offer.h"

namespace fs = std::filesystem;
namespace su = bots::setup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static su::StorageFacts Sd() {  // an SD card in a Deck: mounted under /run/media, writable, not the home or root device
  su::StorageFacts f;
  f.under_media_mount = true;
  f.same_as_home_or_boot = false;
  f.read_only = false;
  return f;
}

int main() {
  // Storage: the conservative rule.
  {
    su::StorageFacts f = Sd();
    CHECK(su::IsRemovableOrExternal(f));
    f = Sd(); f.read_only = true;           CHECK(!su::IsRemovableOrExternal(f));  // a mounted disk image, a locked card
    f = Sd(); f.network = true;             CHECK(!su::IsRemovableOrExternal(f));
    f = Sd(); f.same_as_home_or_boot = true; CHECK(!su::IsRemovableOrExternal(f)); // internal storage, whatever the path
    f = Sd(); f.under_media_mount = false;  CHECK(!su::IsRemovableOrExternal(f));  // a fixed disk at some other mount
    f = Sd(); f.under_media_mount = false; f.device_removable = true; CHECK(su::IsRemovableOrExternal(f));  // a USB stick
    f = Sd(); f.device_internal = true;     CHECK(!su::IsRemovableOrExternal(f));  // built-in disk mounted under /Volumes
    f = Sd(); f.device_internal = true; f.device_removable = true; CHECK(su::IsRemovableOrExternal(f));
    CHECK(!su::IsRemovableOrExternal(su::StorageFacts{}));  // nothing known: no offer
  }
  CHECK(su::UnderMediaMount("/run/media/deck/SD/Games/Skullburglar.AppImage"));
  CHECK(su::UnderMediaMount("/media/player/USB"));
  CHECK(!su::UnderMediaMount("/run/mediaX/a"));
  CHECK(!su::UnderMediaMount("/data/Games"));
  CHECK(!su::UnderMediaMount("/mnt/data"));

  // The decision.
  {
    su::OfferInput in;
    in.removable_or_external = true;
    CHECK(su::DecideOffer(in) == su::OfferDecision::kShow);
    auto with = [&](auto&& edit) { su::OfferInput i = in; edit(i); return su::DecideOffer(i); };
    CHECK(with([](su::OfferInput& i) { i.portable_already = true; }) == su::OfferDecision::kAlreadyPortable);
    CHECK(with([](su::OfferInput& i) { i.answered = true; }) == su::OfferDecision::kAlreadyAnswered);
    CHECK(with([](su::OfferInput& i) { i.removable_or_external = false; }) == su::OfferDecision::kInternalStorage);
    CHECK(with([](su::OfferInput& i) { i.existing.shared_game_files = true; }) == su::OfferDecision::kExistingInstall);
    CHECK(with([](su::OfferInput& i) { i.existing.user_data = true; }) == su::OfferDecision::kExistingInstall);
    CHECK(with([](su::OfferInput& i) { i.existing.game_next_to_program = true; }) == su::OfferDecision::kExistingInstall);
    CHECK(with([](su::OfferInput& i) { i.existing.settings_next_to_program = true; }) == su::OfferDecision::kExistingInstall);
    CHECK(with([](su::OfferInput& i) { i.launcher_visible = false; }) == su::OfferDecision::kNoLauncher);
    CHECK(with([](su::OfferInput& i) { i.game_dir_given = true; }) == su::OfferDecision::kCommandLine);
    CHECK(with([](su::OfferInput& i) { i.disabled_by_env = true; }) == su::OfferDecision::kDisabled);
    // Order: an answer or portable mode beats everything else; internal storage is never asked even with no install.
    CHECK(with([](su::OfferInput& i) { i.portable_already = true; i.existing.user_data = true; }) == su::OfferDecision::kAlreadyPortable);
    su::OfferInput internal;
    CHECK(su::DecideOffer(internal) == su::OfferDecision::kInternalStorage);
    for (int d = 0; d <= int(su::OfferDecision::kDisabled); ++d) CHECK(*su::WhyNoOffer(su::OfferDecision(d)));
  }

  // The answer files, in a scratch folder.
  {
    const fs::path dir = fs::temp_directory_path() / ("bots-offer-test-" + std::to_string(std::rand()));
    fs::create_directories(dir);
    CHECK(!su::IsAnswered(dir));
    CHECK(su::AnswerNo(dir).empty());
    CHECK(su::IsAnswered(dir));
    CHECK(fs::is_regular_file(dir / "portable-declined.txt"));
    CHECK(!fs::exists(dir / "portable.txt"));
    CHECK(su::AnswerYes(dir).empty());  // changing one's mind: portable.txt appears, the declined note goes
    CHECK(fs::is_regular_file(dir / "portable.txt"));
    CHECK(!fs::exists(dir / "portable-declined.txt"));
    // portable.txt is what portable.h's resolver reads.
    const auto r = su::ResolveDataRoot("", dir, "", "/cwd", [](const fs::path& p) { return fs::is_regular_file(p); });
    CHECK(r.source == su::DataSource::kPortableFile && r.root == dir / "bots-data");
    // A folder that can't be written says so, and nothing is created.
    CHECK(!su::AnswerYes(dir / "missing" / "deeper").empty());
    CHECK(!su::AnswerNo(dir / "missing" / "deeper").empty());
    // Existing-install probe.
    fs::create_directories(dir / "shared");
    CHECK(!su::ProbeExistingInstall(dir / "shared", dir / "nodata", dir / "nogame", dir).Any());
    fs::create_directories(dir / "userdata");
    CHECK(!su::ProbeExistingInstall(dir / "shared", dir / "userdata", dir / "nogame", dir).Any());  // empty folder
    std::ofstream(dir / "userdata" / "bots.toml") << "x\n";
    CHECK(su::ProbeExistingInstall(dir / "shared", dir / "userdata", dir / "nogame", dir).user_data);
    std::ofstream(dir / "bots.toml") << "x\n";
    CHECK(su::ProbeExistingInstall(dir / "shared", dir / "nodata", dir / "nogame", dir).settings_next_to_program);
    fs::create_directories(dir / "shared" / "C1Game");
    std::ofstream(dir / "shared" / "default.xex") << "x";
    CHECK(su::ProbeExistingInstall(dir / "shared", dir / "nodata", dir / "nogame", dir / "none").shared_game_files);
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::printf(failures ? "%d FAILED\n" : "portable_offer_test: all passed\n", failures);
  return failures ? 1 : 0;
}
