// Plain-assert tests for src/path_utf8.h. Run: c++ -std=c++20 -I src tests/path_utf8_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "path_utf8.h"

using bots::EnvPath;
using bots::FileUrl;
using bots::HexDecode;
using bots::HexEncode;
using bots::IsValidUtf8;
using bots::SamePath;
using bots::OpenFile;
using bots::PathFromUtf8;
using bots::PathUtf8;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  // A Russian "Documents" folder, as in GitHub issue 25. Written as UTF-8 bytes so the test doesn't depend on the
  // compiler's source or execution character set.
  const std::string docs = "\xD0\x94\xD0\xBE\xD0\xBA\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8B";  // Документы
  const std::string text = "C:/profile/" + docs + "/bots";

  // UTF-8 text -> path -> UTF-8 text is lossless, and never throws whatever the system code page is.
  const std::filesystem::path p = PathFromUtf8(text);
  CHECK(PathUtf8(p) == text);
  CHECK(PathUtf8(p.filename()) == "bots");
  CHECK(PathUtf8(p.parent_path().filename()) == docs);
  CHECK(PathUtf8(p / "bots.toml") == text + "/bots.toml");

  // ASCII and empty paths are unchanged.
  CHECK(PathUtf8(PathFromUtf8("plain/path.txt")) == "plain/path.txt");
  CHECK(PathUtf8(std::filesystem::path()).empty());
  CHECK(PathFromUtf8("").empty());

  // Comparison goes through the same conversion on both sides.
  CHECK(PathFromUtf8(text) == PathFromUtf8(text));
  CHECK(PathFromUtf8(text) != PathFromUtf8(text + "x"));

  // A file in a folder with a non-ASCII name can be created, written and read back by its path.
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / PathFromUtf8("utf8path-test-" + docs);
  std::filesystem::remove_all(dir, ec);
  CHECK(std::filesystem::create_directories(dir, ec));
  const std::filesystem::path file = dir / PathFromUtf8(docs + ".txt");
  if (std::FILE* f = OpenFile(file, "wb")) {
    std::fputs("hello", f);
    std::fclose(f);
  } else {
    CHECK(!"OpenFile write");
  }
  {
    std::ifstream in(file, std::ios::binary);
    std::string got;
    in >> got;
    CHECK(got == "hello");
  }
  if (std::FILE* f = OpenFile(file, "rb")) {
    char buf[8] = {};
    CHECK(std::fread(buf, 1, 5, f) == 5);
    CHECK(std::string(buf) == "hello");
    std::fclose(f);
  } else {
    CHECK(!"OpenFile read");
  }
  // The directory listing gives the name back as the UTF-8 text it was created with.
  bool listed = false;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) listed |= PathUtf8(e.path().filename()) == docs + ".txt";
  CHECK(listed);
  std::filesystem::remove_all(dir, ec);

  // Emoji (4-byte UTF-8, a surrogate pair in UTF-16), CJK, combining marks, and awkward ASCII.
  const std::string odd[] = {
      "/home/\xF0\x9F\x8E\xAE games/\xF0\x9F\x93\x81",                // emoji
      "/home/\xE6\x96\x87\xE6\x9B\xB8/\xE3\x83\x95\xE3\x82\xA9",  // CJK
      "/Data/Cafe\xCC\x81/n\xCC\x83",                                 // NFD: e + combining acute
      "/Data/Caf\xC3\xA9/\xC3\xB1",                                   // NFC: precomposed
      "/media/My SD & O'Brien \"x\" 100% #1 ?q",
  };
  for (const std::string& t : odd) {
    CHECK(IsValidUtf8(t));
    CHECK(PathUtf8(PathFromUtf8(t)) == t);
    CHECK(PathUtf8(PathFromUtf8(t).parent_path()) == t.substr(0, t.rfind('/')));
  }

  // NFC and NFD spell the same name with different bytes: the paths differ as strings, and SamePath tells them apart
  // from a different folder only through the file system (macOS finds the one folder either way; Linux doesn't).
  const std::string nfc = "caf\xC3\xA9", nfd = "cafe\xCC\x81";
  CHECK(PathFromUtf8(nfc) != PathFromUtf8(nfd));
  CHECK(SamePath(PathFromUtf8(nfc), PathFromUtf8(nfc)));
  CHECK(!SamePath(PathFromUtf8(nfc), PathFromUtf8("other")));
  {
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec) / "path-utf8-nfc-test";
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base / PathFromUtf8(nfc), ec);
    const bool found_by_nfd = std::filesystem::exists(base / PathFromUtf8(nfd), ec);
#if defined(__APPLE__)
    CHECK(found_by_nfd);  // APFS and HFS+ compare names ignoring the normalization form
    CHECK(SamePath(base / PathFromUtf8(nfc), base / PathFromUtf8(nfd)));
#else
    CHECK(!found_by_nfd);
    CHECK(!SamePath(base / PathFromUtf8(nfc), base / PathFromUtf8(nfd)));
#endif
    std::filesystem::remove_all(base, ec);
  }

  // A long path: 3 folders of 240 bytes each (over 700 bytes in all: macOS allows 1024 for a whole path), under the 255-byte name limit.
  {
    std::string name;
    for (int i = 0; i < 120; ++i) name += "\xD0\xB4";  // 120 Cyrillic letters = 240 bytes
    std::filesystem::path deep = std::filesystem::temp_directory_path(ec) / "path-utf8-long-test";
    std::filesystem::remove_all(deep, ec);
    for (int i = 0; i < 3; ++i) deep /= PathFromUtf8(name);
    CHECK(PathUtf8(deep).size() > 720);
    CHECK(std::filesystem::create_directories(deep, ec));
    if (std::FILE* f = OpenFile(deep / "bots.toml", "wb")) {
      std::fputs("x", f);
      std::fclose(f);
    } else {
      CHECK(!"OpenFile in a long path");
    }
    CHECK(std::filesystem::exists(deep / "bots.toml", ec));
    CHECK(PathUtf8(deep.filename()) == name);
    std::filesystem::remove_all(std::filesystem::temp_directory_path(ec) / "path-utf8-long-test", ec);
  }

#if !defined(_WIN32)
  // Linux (and macOS) keep file names as bytes, so a folder name need not be UTF-8: a Latin-1 "cafe" from an old SD
  // card is legal. The helpers pass the bytes through unchanged, so they can be handed back to the system (APFS
  // refuses to create such a name, so only the string round trip runs there).
  {
    const std::string latin1 = "/mnt/caf\xE9/game";
    CHECK(!IsValidUtf8(latin1));
    CHECK(PathUtf8(PathFromUtf8(latin1)) == latin1);
    CHECK(PathUtf8(PathFromUtf8(latin1).parent_path().filename()) == "caf\xE9");
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / PathFromUtf8("path-utf8-caf\xE9");
    std::filesystem::remove_all(dir, ec);
    if (std::filesystem::create_directories(dir, ec)) {
      if (std::FILE* f = OpenFile(dir / "a.txt", "wb")) {
        std::fputs("ok", f);
        std::fclose(f);
      } else {
        CHECK(!"OpenFile in a non-UTF-8 folder");
      }
      bool listed = false;
      for (const auto& e : std::filesystem::directory_iterator(dir, ec)) listed |= e.path().filename() == "a.txt";
      CHECK(listed);
      std::filesystem::remove_all(dir, ec);
    }
  }
#endif

  // UTF-8 validation: what toml++ accepts.
  CHECK(IsValidUtf8(""));
  CHECK(IsValidUtf8("ascii"));
  CHECK(IsValidUtf8("\xF0\x9F\x98\x80"));
  CHECK(!IsValidUtf8("caf\xE9"));                // Latin-1
  CHECK(!IsValidUtf8("\xC3"));                   // cut off
  CHECK(!IsValidUtf8("\xF0\x9F\x98"));          // cut off
  CHECK(!IsValidUtf8("\xC0\xAF"));              // overlong "/"
  CHECK(!IsValidUtf8("\xED\xA0\x80"));          // surrogate
  CHECK(!IsValidUtf8("\xF4\x90\x80\x80"));      // past U+10FFFF
  CHECK(!IsValidUtf8("\xFF\xFE"));
  CHECK(!IsValidUtf8("a\x80"));                  // stray continuation byte

  // Hex storage for paths that aren't UTF-8 text.
  {
    const std::string raw = "/mnt/caf\xE9\xFF\x00x";
    bool ok = false;
    CHECK(HexDecode(HexEncode(raw), &ok) == raw && ok);
    HexDecode("abc", &ok);
    CHECK(!ok);
    HexDecode("zz", &ok);
    CHECK(!ok);
    CHECK(HexEncode("\xE9") == "e9");
  }

  // file:// URLs for the "Open log" button.
  CHECK(FileUrl("/a b/c") == "file:///a%20b/c");
  CHECK(FileUrl("/100%/#1/?x") == "file:///100%25/%231/%3Fx");
  CHECK(FileUrl("/\xD0\x94/\xF0\x9F\x8E\xAE") == "file:///%D0%94/%F0%9F%8E%AE");
  CHECK(FileUrl("C:/Data/x") == "file:///C:/Data/x");
  CHECK(FileUrl("/O'Brien & co") == "file:///O%27Brien%20%26%20co");

  // Long Windows paths: extended-length form only for long absolute paths, canonical, never for short ones.
  {
    using bots::ExtendedPathText; using bots::PlainPathText; using bots::kLongPathThreshold;
    const std::string pad(60, 'P');
    const std::string deep = "C:\\lg\\" + pad + "\\" + pad + "\\" + pad;  // 195 characters
    CHECK(deep.size() > kLongPathThreshold);
    CHECK(ExtendedPathText(deep) == "\\\\?\\" + deep);
    CHECK(ExtendedPathText("C:/lg/" + pad + "/" + pad + "/" + pad) == "\\\\?\\" + deep);  // slashes become backslashes
    CHECK(ExtendedPathText(deep + "\\x\\..\\.\\y\\\\") == "\\\\?\\" + deep + "\\y");        // . .. and doubled separators
    CHECK(ExtendedPathText("C:\\short\\game") == "C:\\short\\game");                        // short: untouched
    CHECK(ExtendedPathText("C:/short/game") == "C:/short/game");
    CHECK(ExtendedPathText(deep, 300) == deep);                                              // threshold
    CHECK(ExtendedPathText("\\\\?\\" + deep) == "\\\\?\\" + deep);                          // already extended
    CHECK(ExtendedPathText(pad + "\\" + pad + "\\" + pad + "\\" + pad) == pad + "\\" + pad + "\\" + pad + "\\" + pad);  // relative
    CHECK(ExtendedPathText("/home/" + pad + "/" + pad + "/" + pad + "/" + pad).starts_with("/home"));        // Unix path
    CHECK(ExtendedPathText("\\\\srv\\share\\" + pad + "\\" + pad + "\\" + pad) ==
          "\\\\?\\UNC\\srv\\share\\" + pad + "\\" + pad + "\\" + pad);
    CHECK(PlainPathText("\\\\?\\" + deep) == deep);
    CHECK(PlainPathText("\\\\?\\UNC\\srv\\share\\x") == "\\\\srv\\share\\x");
    CHECK(PlainPathText("C:\\x") == "C:\\x");
    CHECK(PlainPathText("\\\\?\\Volume{1}\\x") == "\\\\?\\Volume{1}\\x");  // not a drive form: left alone
    CHECK(PlainPathText(ExtendedPathText(deep)) == deep);
  }

  // An unset variable is an empty path.
  CHECK(EnvPath("BOTS_PATH_UTF8_TEST_UNSET_VARIABLE").empty());

  if (failures) return 1;
  std::printf("ok\n");
  return 0;
}
