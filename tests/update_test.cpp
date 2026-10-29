// Plain-assert tests for the launcher's self-update logic (src/setup/update_logic.*).
// Run: c++ -std=c++20 -Wall -Werror -I src tests/update_test.cpp src/setup/update_logic.cpp src/setup/ed25519.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "setup/ed25519.h"
#include "setup/update_logic.h"

using namespace bots::setup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static const std::string kSha(64, 'a');

static std::string Releases(const std::string& latest_downloads, const std::string& extra = "") {
  return R"({"latest": "1.2.0", "page": "https://site.example/", "updated": "2026-10-29T00:00:00Z", "versions": [
    {"version": "1.2.0", "date": "2026-10-29", "notes": ["Fixed \"things\"."], "available": true, "downloads": {)" +
         latest_downloads + R"(}},
    {"version": "1.1.0", "available": true, "downloads": {"windows": {"file": "old.exe", "size": 5, "sha256": ")" + kSha +
         R"(", "url": "/dl/1.1.0/old.exe"}}}]})" + extra;
}

static std::string Dl(const std::string& key, const std::string& file, const std::string& sha, const std::string& size,
                      const std::string& url) {
  return "\"" + key + "\": {\"file\": \"" + file + "\", \"size\": " + size + ", \"sha256\": \"" + sha +
         "\", \"url\": \"" + url + "\"}";
}

int main() {
  // Versions.
  CHECK(IsNewerVersion("1.10.0", "1.9.9"));
  CHECK(!IsNewerVersion("1.0.0", "1.0.0"));
  CHECK(!IsNewerVersion("1.0", "1.0.0"));
  CHECK(IsNewerVersion("v2.0.0", "1.99.99"));
  CHECK(!IsNewerVersion("0.9.0", "1.0.0-dev.abc"));
  CHECK(IsNewerVersion("1.0.1", "1.0.0-dev.abc"));
  CHECK(!HasVersionNumber("dev") && HasVersionNumber("0.4.0"));

  // The releases file: the newest version's packages, relative addresses resolved on the file's own site.
  const std::string base = "https://site.example/version.json";
  {
    std::string body = Releases(Dl("windows", "sb-1.2.0-setup.exe", kSha, "29084729", "/dl/1.2.0/sb-1.2.0-setup.exe") + "," +
                                Dl("macos", "sb-1.2.0.dmg", kSha, "58847676", "https://site.example/dl/1.2.0/sb-1.2.0.dmg"));
    UpdateInfo i = ParseReleases(body, base);
    CHECK(i.error.empty() && i.latest == "1.2.0" && i.page == "https://site.example/");
    CHECK(i.downloads.size() == 2);
    CHECK(i.downloads["windows"].url == "https://site.example/dl/1.2.0/sb-1.2.0-setup.exe");
    CHECK(i.downloads["windows"].size == 29084729 && i.downloads["windows"].file == "sb-1.2.0-setup.exe");
    CHECK(i.downloads["macos"].sha256 == kSha);
    CHECK(!i.downloads.count("old.exe"));  // the 1.1.0 entry is not the newest
  }
  // Anything unsafe in an entry drops that entry only.
  {
    std::string body = Releases(
        Dl("windows", "../evil.exe", kSha, "10", "/dl/x") + "," +                                  // path in the name
        Dl("macos", "a.dmg", std::string(64, 'A'), "10", "/dl/a.dmg") + "," +                       // upper-case hash
        Dl("linux_tar", "a.tar.xz", kSha, "10", "https://evil.example/a.tar.xz") + "," +            // other site
        Dl("linux_appimage", "a.AppImage", "abc", "10", "/dl/a") + "," +                            // short hash
        Dl("ios", "a.ipa", kSha, "0", "/dl/a.ipa") + "," +                                          // empty
        Dl("tvos", "a.ipa", kSha, "3000000000", "/dl/a.ipa") + "," +                                // too big
        Dl("android", "ok.apk", kSha, "10", "http://site.example/dl/ok.apk") + "," +                // downgrade to http
        Dl("good", "good.bin", kSha, "10", "//evil.example/dl/good.bin"));                          // scheme-relative
    UpdateInfo i = ParseReleases(body, base);
    CHECK(i.latest == "1.2.0");
    CHECK(i.downloads.empty());
  }
  {
    UpdateInfo i = ParseReleases(Releases(Dl("windows", "w.exe", kSha, "10", "https://user@site.example/w.exe")), base);
    CHECK(i.downloads.empty());  // userinfo trick
    i = ParseReleases(Releases(Dl("windows", "w.exe", kSha, "10", "https://SITE.example/w.exe")), base);
    CHECK(i.downloads.size() == 1);  // host case doesn't matter
  }
  // A local test server speaks plain http, and only then is http allowed.
  {
    std::string body = Releases(Dl("macos", "a.dmg", kSha, "10", "/dl/a.dmg"));
    UpdateInfo i = ParseReleases(body, "http://127.0.0.1:8765/version.json");
    CHECK(i.downloads.size() == 1 && i.downloads["macos"].url == "http://127.0.0.1:8765/dl/a.dmg");
  }
  // Unavailable newest release (its packages were pruned): no downloads, the page link still works.
  {
    std::string body = R"({"latest": "2.0.0", "page": "https://site.example/", "versions": [{"version": "2.0.0", "available": false, "downloads": {)" +
                       Dl("windows", "w.exe", kSha, "10", "/dl/w.exe") + "}}]}";
    UpdateInfo i = ParseReleases(body, base);
    CHECK(i.latest == "2.0.0" && i.downloads.empty());
  }
  // The old two-field file still works; junk doesn't.
  {
    UpdateInfo i = ParseReleases(R"({"latest": "0.4.0", "page": "https://s.example/"})", base);
    CHECK(i.latest == "0.4.0" && i.downloads.empty() && i.error.empty());
    i = ParseReleases("not json", base);
    CHECK(i.latest.empty() && !i.error.empty());
    i = ParseReleases(R"({"latest": "0.4.0", "versions": [{"version": )", base);  // truncated: not trusted at all
    CHECK(i.latest.empty() && i.downloads.empty() && !i.error.empty());
    i = ParseReleases(R"({"latest": "0.4.0", "page": "https://s.example/"} trailing)", base);
    CHECK(i.latest.empty() && !i.error.empty());
  }

  // Which install, which package.
  CHECK(InstallKindFor("windows", "C:\\Games\\Skullburglar", "", true) == InstallKind::kWindowsSetup);
  CHECK(InstallKindFor("windows", "C:\\Users\\x\\Downloads\\sb-unzipped", "") == InstallKind::kNone);  // not made by the setup
  CHECK(InstallKindFor("windows", "", "", true) == InstallKind::kNone);
  CHECK(InstallKindFor("macos", "/Applications/Skullburglar.app/Contents/MacOS", "") == InstallKind::kMacBundle);
  CHECK(InstallKindFor("macos", "/Applications/Extras/Skullburglar 2.app/Contents/MacOS", "") == InstallKind::kMacBundle);
  CHECK(InstallKindFor("macos", "/Volumes/Skullburglar 1.0.0/Skullburglar.app/Contents/MacOS", "") == InstallKind::kNone);
  CHECK(InstallKindFor("macos", "/private/var/folders/x/AppTranslocation/ABC/d/Skullburglar.app/Contents/MacOS", "") ==
        InstallKind::kNone);
  CHECK(MacAppNeedsMove("/Volumes/Skullburglar 1.0.0/Skullburglar.app/Contents/MacOS"));
  CHECK(MacAppNeedsMove("/private/var/folders/x/AppTranslocation/ABC/d/Skullburglar.app/Contents/MacOS"));
  CHECK(!MacAppNeedsMove("/Applications/Skullburglar.app/Contents/MacOS"));
  CHECK(InstallKindFor("macos", "/opt/build/out/bots-dir", "") == InstallKind::kNone);  // not a bundle
  CHECK(InstallKindFor("linux", "/tmp/.mount_abc", "/opt/games/Skullburglar.AppImage") == InstallKind::kLinuxAppImage);
  CHECK(InstallKindFor("linux", "/opt/games/skullburglar-1.0.0-linux-x86_64", "") == InstallKind::kNone);  // tarball: link only
  CHECK(InstallKindFor("freebsd", "/x", "") == InstallKind::kNone);
  {
    UpdateInfo i = ParseReleases(Releases(Dl("windows", "w.exe", kSha, "10", "/dl/w.exe") + "," +
                                          Dl("linux_tar", "t.tar.xz", kSha, "20", "/dl/t.tar.xz")),
                                 base);
    CHECK(PickAsset(i, InstallKind::kWindowsSetup) && PickAsset(i, InstallKind::kWindowsSetup)->file == "w.exe");
    CHECK(!PickAsset(i, InstallKind::kMacBundle));      // this release has no disk image
    CHECK(!PickAsset(i, InstallKind::kLinuxAppImage));  // nor an AppImage
    CHECK(!PickAsset(i, InstallKind::kNone));
  }

  // Ed25519: RFC 8032 section 7.1 test vectors 1 to 3, and tampering.
  {
    struct V { const char *pk, *msg, *sig; };
    const V vs[] = {
        {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (const V& v : vs) {
      uint8_t pk[32], sig[64], msg[8];
      const size_t mlen = std::strlen(v.msg) / 2;
      CHECK(HexToBytes(v.pk, pk, 32) && HexToBytes(v.sig, sig, 64) && HexToBytes(v.msg, msg, mlen));
      CHECK(Ed25519Verify(sig, msg, mlen, pk));
      uint8_t bad[64];
      std::memcpy(bad, sig, 64);
      bad[10] ^= 1;  // R changed
      CHECK(!Ed25519Verify(bad, msg, mlen, pk));
      std::memcpy(bad, sig, 64);
      bad[40] ^= 1;  // S changed
      CHECK(!Ed25519Verify(bad, msg, mlen, pk));
    }
    uint8_t pk[32], sig[64];
    HexToBytes(vs[1].pk, pk, 32), HexToBytes(vs[1].sig, sig, 64);
    uint8_t msg = 0x72;
    CHECK(Ed25519Verify(sig, &msg, 1, pk));
    uint8_t wrong = 0x73;  // another message
    CHECK(!Ed25519Verify(sig, &wrong, 1, pk));
    // S + L is the same signature mathematically but must be refused (malleability).
    static const uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
    uint8_t mall[64];
    std::memcpy(mall, sig, 64);
    int carry = 0;
    for (int i = 0; i < 32; ++i) {
      int v = mall[32 + i] + kL[i] + carry;
      mall[32 + i] = uint8_t(v);
      carry = v >> 8;
    }
    CHECK(!Ed25519Verify(mall, &msg, 1, pk));
    uint8_t zero_key[32] = {0};
    zero_key[0] = 2;  // not a curve point (x^2 has no root) or a weak one: must not verify anything
    CHECK(!Ed25519Verify(sig, &msg, 1, zero_key));
    CHECK(!HexToBytes("zz", pk, 1) && !HexToBytes("abc", pk, 1) && HexToBytes("0aFf", pk, 2) && pk[0] == 0x0a && pk[1] == 0xff);
  }

  // The signed releases file: one throwaway test key (only its public half is here), a signature over the newest
  // release's version and packages (SigningMessage), made by tools/ci/ed25519_sign.py.
  {
    const std::string pub = "e799253b4aa5ed5f388914728cfb14b80fa8b1ea1137322d6d2d2ec824e136aa";
    const std::string other_pub = "02d27e604f0f448146dc86f51a0e48df0bcf979da71d8be0d3bec8c2133905c1";
    const std::string sig =
        "db15e072e9b8767e3e6258a06aee1dac9d12a5980ae232ddef4cf321c14a7c6bfe42b73057995a446c788760e4372c2e5d9da3a7cdcf0d613c15ff0adcd9770a";
    auto body = [&](const std::string& signature, const std::string& mac_size = "58847676") {
      return std::string(R"({"latest": "1.2.0", "page": "https://site.example/", "versions": [{"version": "1.2.0", "available": true, "signature": ")") +
             signature + R"(", "downloads": {)" +
             Dl("windows", "sb-1.2.0-setup.exe", kSha, "29084729", "/dl/1.2.0/sb-1.2.0-setup.exe") + "," +
             Dl("macos", "sb-1.2.0.dmg", kSha, mac_size, "/dl/1.2.0/sb-1.2.0.dmg") + "}}]}";
    };
    UpdateInfo i = ParseReleases(body(sig), base, pub);
    CHECK(i.signed_ok && i.downloads.size() == 2);
    CHECK(i.downloads["macos"].signed_ok && i.downloads["macos"].version == "1.2.0");
    CHECK(SigningMessage("1.2.0", {"windows a 1 x\n", "macos b 2 y\n"}) ==
          "skullburglar-update-v1\nversion=1.2.0\nmacos b 2 y\nwindows a 1 x\n");  // sorted
    // No key built in, a different key, no signature, a damaged one, or any listed value changed: not signed.
    CHECK(!ParseReleases(body(sig), base).signed_ok);
    CHECK(!ParseReleases(body(sig), base, other_pub).signed_ok);
    CHECK(!ParseReleases(body(sig), base, "not hex").signed_ok);
    CHECK(!ParseReleases(body(""), base, pub).signed_ok);
    CHECK(!ParseReleases(body(sig.substr(0, 126) + "00"), base, pub).signed_ok);
    CHECK(!ParseReleases(body(sig, "58847677"), base, pub).signed_ok);  // size
    UpdateInfo t = ParseReleases(body(sig), base, pub);
    CHECK(t.downloads.size() == 2 && !ParseReleases(body(sig, "58847676").replace(body(sig).find(kSha), 1, "b"), base, pub).signed_ok);
    // Entries the signature can't cover (bad name) make the whole release unsigned even if the rest verifies.
    std::string extra = body(sig);
    extra.insert(extra.find("\"windows\""), Dl("ios", "../x.ipa", kSha, "10", "/dl/x") + ",");
    CHECK(!ParseReleases(extra, base, pub).signed_ok);
    // The availability flag isn't signed, so a signed install needs it explicitly true (missing or false: not signed).
    {
      std::string b = body(sig);
      const std::string flag = R"("available": true, )";
      std::string missing = b;
      missing.erase(missing.find(flag), flag.size());
      CHECK(!ParseReleases(missing, base, pub).signed_ok);
      std::string off = b;
      off.replace(off.find("true"), 4, "false");
      UpdateInfo u = ParseReleases(off, base, pub);
      CHECK(!u.signed_ok && u.downloads.empty());
    }
    // The version string: [0-9A-Za-z.+-] only, so it can't smuggle a newline, a path or a space into the signed text.
    for (const char* bad : {"1.2.0\\nx", "1.2.0/../x", "1.2 0", "1.2.0_rc", "1.2.0\\u00e9"}) {
      std::string v = std::string(R"({"latest": ")") + bad + R"(", "page": "https://s.example/"})";
      UpdateInfo r = ParseReleases(v, base);
      CHECK(r.latest.empty() && !r.error.empty());
    }
    CHECK(ParseReleases(R"({"latest": "1.2.0-rc.1+build5"})", base).latest == "1.2.0-rc.1+build5");
    CHECK(ParseReleases(std::string(R"({"latest": "1.)") + std::string(70, '0') + R"("})", base).latest.empty());
    // "size": a number that isn't one of 1..2e9 (NaN included) never gives an entry.
    CHECK(ParseReleases(Releases(Dl("windows", "w.exe", kSha, "NaN", "/dl/w.exe")), base).downloads.empty());
    CHECK(ParseReleases(Releases(Dl("windows", "w.exe", kSha, "-5", "/dl/w.exe")), base).downloads.empty());
    CHECK(ParseReleases(Releases(Dl("windows", "w.exe", kSha, "1e3", "/dl/w.exe")), base).downloads.size() == 1);
  }

  // SHA-256 against known answers, including the block-boundary lengths.
  CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(Sha256Hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  CHECK(Sha256Hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  CHECK(Sha256Hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  {
    Sha256 h;  // streamed in odd pieces: a million 'a'
    std::string chunk(997, 'a');
    size_t left = 1000000;
    while (left) {
      size_t n = std::min(left, chunk.size());
      h.Update(chunk.data(), n);
      left -= n;
    }
    CHECK(h.Final() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }

  // Verification: size first, then the hash; a missing file fails.
  {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("update_test_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    fs::path f = dir / "pkg.bin";
    { std::ofstream(f, std::ios::binary) << "abc"; }
    Asset a;
    a.size = 3;
    a.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(VerifyDownload(f, a).empty());
    a.size = 4;
    CHECK(!VerifyDownload(f, a).empty());  // truncated or padded
    a.size = 3;
    a.sha256 = kSha;
    CHECK(!VerifyDownload(f, a).empty());  // same size, different bytes
    CHECK(!VerifyDownload(dir / "none", a).empty());
    CHECK(Sha256File(f) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fs::remove_all(dir);
  }

  if (failures) std::printf("%d failures\n", failures);
  else std::printf("update_test: all passed\n");
  return failures ? 1 : 0;
}
