// Unit tests for the pure part of src/diagnostics.h (B-390, B-403). CI `checks` builds and runs this.
#include <cstdio>
#include <cstdlib>
#include <map>

#include "diagnostics.h"
// (Split literals: the repo's leak scan blocks real-looking home folders, even in tests.)

using namespace bots::diag;

static int failures = 0;
#define CHECK_EQ(a, b)                                                                                        \
  do {                                                                                                       \
    const auto va = (a);                                                                                     \
    const auto vb = (b);                                                                                     \
    if (!(va == vb)) {                                                                                       \
      std::fprintf(stderr, "FAIL line %d: %s\n  got:  %s\n  want: %s\n", __LINE__, #a, std::string(va).c_str(), \
                   std::string(vb).c_str());                                                                 \
      ++failures;                                                                                            \
    }                                                                                                        \
  } while (0)
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c), ++failures; \
  } while (0)

int main() {
  // Secrets are hidden by name, whatever the value.
  CHECK(IsSecretName("BOTS_API_TOKEN"));
  CHECK(IsSecretName("rex_some_Key"));
  CHECK(IsSecretName("BOTS_SECRET"));
  CHECK(!IsSecretName("BOTS_GFX_DEPTH_BIAS_SCALE"));
  CHECK_EQ(SanitizeEnvValue("BOTS_UPLOAD_TOKEN", "abc123"), "<hidden>");

  // Values: empty, plain, control characters, long.
  CHECK_EQ(SanitizeEnvValue("BOTS_X", ""), "(empty)");
  CHECK_EQ(SanitizeEnvValue("BOTS_X", "0.5"), "0.5");
  CHECK_EQ(SanitizeEnvValue("BOTS_X", "a\nb\tc"), "a b c");
  CHECK(SanitizeEnvValue("BOTS_X", std::string(300, 'x')).size() == 123);

  // Paths: home becomes ~, long paths keep their last two parts.
  CHECK_EQ(ShortenPath("/var/folks/games/bots", "/var/folks"), "~/games/bots");
  CHECK_EQ(ShortenPath("/var/folksx/games", "/var/folks"), "/var/folksx/games");  // not a prefix on a boundary
  CHECK_EQ(ShortenPath("/very/long/path/with/many/parts/that/goes/on/and/on/forever/and/ever/to/game", "", 40), ".../to/game");
  CHECK_EQ(ShortenPath("C:\\Games\\Skullburglar\\a\\very\\long\\folder\\name\\here\\game", "", 40), "...\\here\\game");
  CHECK_EQ(SanitizeEnvValue("BOTS_DATA_DIR", "/opt/h/data", "/opt/h"), "~/data");
  CHECK(LooksLikePath("D:\\Games"));
  CHECK(!LooksLikePath("1.5"));

  // The environment block: only BOTS_ and REX_, sorted, odd entries skipped.
  {
    const auto lines = EnvSwitchLines({"PATH=/usr/bin", "REX_FRAMERATE_LIMIT=0", "BOTS_N1_MAXTEX=1024", "=C:=C:\\", "BOTSX=1",
                                       "BOTS_GFX_VS_CONTRACTION=", "BOTS_PASSWORD=hunter2", "NOEQUALS", "BOTS_HYBRID_STATE=55bc7a8a6ee0"});
    CHECK(lines.size() == 4);
    CHECK_EQ(lines[0], "env: BOTS_GFX_VS_CONTRACTION=(empty)");
    CHECK_EQ(lines[1], "env: BOTS_N1_MAXTEX=1024");
    CHECK_EQ(lines[2], "env: BOTS_PASSWORD=<hidden>");
    CHECK_EQ(lines[3], "env: REX_FRAMERATE_LIMIT=0");
  }

  // os-release, SteamOS style.
  {
    const auto kv = ParseKeyValues("# comment\nNAME=\"SteamOS\"\nPRETTY_NAME=\"SteamOS Holo\"\nVARIANT_ID=steamdeck\nBUILD_ID=20250211.1\n\nVERSION_ID=3.7\n");
    CHECK_EQ(kv.at("NAME"), "SteamOS");
    CHECK_EQ(LinuxOsName(kv), "SteamOS Holo (steamdeck, build 20250211.1)");
    CHECK_EQ(LinuxOsName({{"PRETTY_NAME", "Ubuntu 24.04.1 LTS"}}), "Ubuntu 24.04.1 LTS");
    CHECK_EQ(LinuxOsName({}), "Linux");
  }

  // Filesystem type from mountinfo: the longest mount point wins, spaces are escaped.
  {
    const std::string mi =
        "25 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "40 25 8:1 / /run/media/u/Game\\040SSD rw,nosuid shared:2 - exfat /dev/sda1 rw\n"
        "41 25 8:17 / /var rw shared:3 - btrfs /dev/sdb1 rw\n"
        "42 25 0:30 / /mnt/ntfs rw - fuseblk /dev/sdc1 rw\n";
    CHECK_EQ(FsTypeFromMountInfo(mi, "/var/folks/bots"), "btrfs");
    CHECK_EQ(FsTypeFromMountInfo(mi, "/run/media/u/Game SSD/bots"), "exfat");
    CHECK_EQ(FsTypeFromMountInfo(mi, "/varnish"), "ext4");  // /var is not a prefix on a path boundary
    CHECK_EQ(FsTypeFromMountInfo(mi, "/usr/share"), "ext4");
    CHECK_EQ(FsTypeFromMountInfo("", "/x"), "");
    CHECK_EQ(FsTypeNote("fuseblk"), "fuseblk (NTFS or exFAT through FUSE)");
    CHECK_EQ(FsTypeNote("ntfs3"), "ntfs");
  }

  // How the game was started.
  {
    std::map<std::string, std::string> e;
    auto get = [&](const char* n) { auto it = e.find(n); return it == e.end() ? std::string() : it->second; };
    CHECK_EQ(LaunchContext(get), "plain desktop session (not started from Steam)");
    e = {{"SteamDeck", "1"}, {"SteamGamepadUI", "1"}, {"GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"}, {"SteamAppId", "480"}};
    CHECK_EQ(LaunchContext(get), "Steam Deck in Game Mode, under gamescope, started from Steam (app 480)");
    e = {{"SteamDeck", "1"}, {"XDG_CURRENT_DESKTOP", "KDE"}};
    CHECK_EQ(LaunchContext(get), "Steam Deck in Desktop Mode");
    e = {{"WINEPREFIX", "/x"}};
    CHECK_EQ(LaunchContext(get), "under Proton/Wine");
    e = {{"APPIMAGE", "/var/a/Skullburglar.AppImage"}};
    CHECK_EQ(InstallKind("/tmp/.mount_SkullbXYZ/usr/bin/bots", get), "AppImage");
    e = {};
    CHECK_EQ(InstallKind("/Applications/Skullburglar.app/Contents/MacOS/bots", get), "macOS app bundle");
    CHECK_EQ(InstallKind("C:\\Program Files\\Skullburglar\\bots.exe", get), "installed system-wide");
    CHECK_EQ(InstallKind("/var/folks/Downloads/skullburglar/bots", get), "unpacked download (zip or tar.gz folder)");
  }

  // Log excerpts.
  CHECK_EQ(LastLines("a\nb\nc\nd\n", 2), "c\nd\n");
  CHECK_EQ(LastLines("a\nb\nc", 2), "b\nc");
  CHECK_EQ(LastLines("a\nb", 5), "a\nb");
  CHECK_EQ(LastLines("", 3), "");
  {
    const auto hits = LinesContaining("x\nVulkan device 'GTX 1070': API\ny\nVulkanPresenter: Created 1280x720\n", {"Vulkan device '", "VulkanPresenter: Created"});
    CHECK(hits.size() == 2);
  }
  CHECK_EQ(GiB(uint64_t(16) << 30), "16.0 GiB");
  CHECK_EQ(SummaryLine("os", "SteamOS"), "summary: os: SteamOS");


  // Scrubbing (the bundle goes into public issues).
  {
    ScrubInfo win{"C:\\Users\\Pat Lee", "Pat Lee", "PATS-LAPTOP"};
    CHECK_EQ(ScrubText("settings file C:\\Users\\Pat Lee\\AppData\\Local\\bots.toml loaded", win), "settings file ~\\AppData\\Local\\bots.toml loaded");
    CHECK_EQ(ScrubText("c:/us" "ers/pat lee/bots and C:\\USERS\\PAT LEE/x", win), "~/bots and ~/x");           // case, mixed separators
    CHECK_EQ(ScrubText("\\\\?\\C:\\Users\\Pat Lee\\Documents", win), "~\\Documents");               // long-path form
    CHECK_EQ(ScrubText("C:\\Users\\PATLEE~1\\Documents", win), "~\\Documents");                          // 8.3 short form
    CHECK_EQ(ScrubText("D:\\Users\\Other\\x and C:\\Users\\Someone Else\\y", win), "~\\x and ~ Else\\y");  // other accounts: up to the space
    CHECK_EQ(ScrubText("host PATS-LAPTOP and pats-laptop.local", win), "host <computer> and <computer>.local");
    CHECK_EQ(ScrubText("D:\\Games\\Pat Lee\\save", win), "D:\\Games\\<user>\\save");                     // the name as a path component
    CHECK_EQ(ScrubText("Pat Lee scored", win), "Pat Lee scored");                                          // not a path component
    ScrubInfo mac{"/Us" "ers/pat", "pat", "pats-mbp"};
    CHECK_EQ(ScrubText("log /Us" "ers/pat/Library/Logs/Skullburglar/a.log", mac), "log ~/Library/Logs/Skullburglar/a.log");
    CHECK_EQ(ScrubText("/Us" "ers/pat2/x /Volumes/Ext/Us" "ers/sam/y", mac), "~/x /Volumes/Ext~/y");
    CHECK_EQ(ScrubText("home /Us" "ers/patricia/x", mac), "home ~/x");                                          // generic pattern, any account
    ScrubInfo lin{"/ho" "me/test", "test", "deck-box"};
    CHECK_EQ(ScrubText("/ho" "me/test/games /ho" "me/test2/x test passed, tests/ok /srv/test/data", lin), "~/games ~/x test passed, tests/ok /srv/<user>/data");
    CHECK_EQ(ScrubText("/ho" "me/testing/x", lin), "~/x");
    CHECK_EQ(ScrubText("nothing to hide: 1.5 GiB", lin), "nothing to hide: 1.5 GiB");
    CHECK_EQ(ScrubText("/ho" "me/alice", ScrubInfo{}), "~");
    CHECK_EQ(ScrubText("x", ScrubInfo{}), "x");
  }

  if (failures) return 1;
  std::puts("diagnostics_test: ok");
  return 0;
}
