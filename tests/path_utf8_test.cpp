// Plain-assert tests for src/path_utf8.h. Run: c++ -std=c++20 -I src tests/path_utf8_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "path_utf8.h"

using bots::EnvPath;
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

  // An unset variable is an empty path.
  CHECK(EnvPath("BOTS_PATH_UTF8_TEST_UNSET_VARIABLE").empty());

  if (failures) return 1;
  std::printf("ok\n");
  return 0;
}
