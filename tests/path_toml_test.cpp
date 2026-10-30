// How folder names survive bots.toml (toml++). Needs the SDK's toml++ headers, so it is not in the CI `checks` job:
//   c++ -std=c++20 -I src -I <rexglue-sdk>/thirdparty/tomlplusplus/include tests/path_toml_test.cpp -o /tmp/t && /tmp/t
// Valid UTF-8 names come back byte for byte. toml++ changes text that is not valid UTF-8 when it writes it (and refuses
// it when it reads it), which is why settings.cpp stores such paths as hex (SetConfigPath).
#include <cstdio>
#include <sstream>
#include <string>

#include <toml++/toml.hpp>

#include "path_utf8.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static std::string RoundTrip(const std::string& value, bool* parsed = nullptr) {
  toml::table t;
  t.insert_or_assign("game_data_root", value);
  std::ostringstream out;
  out << t;
  std::istringstream in(out.str());
  try {
    auto back = toml::parse(in, std::string("test"));
    if (parsed) *parsed = true;
    return back["game_data_root"].value_or(std::string("<missing>"));
  } catch (const toml::parse_error&) {
    if (parsed) *parsed = false;
    return {};
  }
}

int main() {
  const std::string valid[] = {
      "/Data/\xD0\x94\xD0\xBE\xD0\xBA/\xE6\x96\x87\xE6\x9B\xB8/\xF0\x9F\x8E\xAE",
      "/Data/Cafe\xCC\x81/Caf\xC3\xA9",
      "/media/My SD & O'Brien \"x\" 100% #1 ?q \\ back\\slash",
      "C:\\Users\\\xD0\x94\xD0\xBE\xD0\xBA",
  };
  for (const auto& v : valid) CHECK(RoundTrip(v) == v);

  // Not valid UTF-8: toml++ does not give the bytes back (it changed or dropped them) ...
  const std::string latin1 = "/mnt/caf\xE9/game", tail = "/mnt/caf\xE9";
  CHECK(RoundTrip(latin1) != latin1);
  CHECK(RoundTrip(tail) != tail);
  // ... and a file that holds the raw bytes does not parse at all.
  {
    std::istringstream in(std::string("game_data_root = '/mnt/caf\xE9/game'\n"));
    bool threw = false;
    try { toml::parse(in, std::string("raw")); } catch (const toml::parse_error&) { threw = true; }
    CHECK(threw);
  }
  // The hex form is plain ASCII, so it always round-trips, and gives the exact bytes back.
  for (const std::string& raw : {latin1, tail, std::string("\xFF\xFE/x")}) {
    bool ok = false;
    CHECK(bots::HexDecode(RoundTrip(bots::HexEncode(raw)), &ok) == raw && ok);
  }

  if (failures) return 1;
  std::printf("ok\n");
  return 0;
}
