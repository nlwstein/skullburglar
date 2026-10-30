// Test range (B-299): stationary targets and a per-hit damage log, for comparing combat between renderers.
// Everything is off unless BOTS_RANGE_DIR is set (the directory the logs go to); with it unset every hook is one
// predictable branch on a static bool. See src/test_range.cpp for the commands and the log format.
//
//   BOTS_RANGE_DIR=<dir>   on; logs go to <dir>/range.jsonl (see `range log`)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bots::testrange {

bool Enabled();

// A `range ...` console command. Returns true if `cmd` was one; console commands it expands into (summons, teleports)
// are appended to `expand` for the caller to run in order.
bool Command(uint8_t* base, const std::string& cmd, std::vector<std::string>& expand);

}  // namespace bots::testrange
