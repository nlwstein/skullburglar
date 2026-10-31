// Diagnostic logging (B-301). BOTS_DIAG=1 turns on the log lines that cost something or only matter when chasing a
// problem: the periodic host report, the per-frame "gfx hitch" / "n1 hitch" lines and the "top GPU spans" report. The
// launcher's "Diagnostic logging" setting sets it for the game it starts (setup/settings.h ApplyDiagnosticLogging).
// The cheap lines stay on always: the frame-time histogram, slow-frame counts, memory and texture totals, the
// once-per-session host snapshot. An explicit threshold variable (BOTS_HITCH_MS, BOTS_N1_HITCH_MS) still wins.
#pragma once

#include <cstdlib>
#include <cstring>

namespace bots {

// "1" (or any value but empty or "0") is on.
inline bool DiagFlagOn(const char* value) { return value && *value && std::strcmp(value, "0") != 0; }

// BOTS_DIAG, read once.
inline bool DiagLogging() {
  static const bool on = DiagFlagOn(std::getenv("BOTS_DIAG"));
  return on;
}

// A logging threshold: the variable's own value when it is set (0 turns the line off), else `default_when_diag` with
// diagnostic logging on, else 0 (off). `diag` is DiagLogging(); a test run (BOTS_INSTR) counts the hitch lines, so it
// passes diag too.
inline double DiagThreshold(const char* env_value, bool diag, double default_when_diag) {
  if (env_value && *env_value) return std::atof(env_value);
  return diag ? default_when_diag : 0.0;
}

// The default for the hitch lines: on with BOTS_DIAG, and in instrumented test runs (BOTS_INSTR counts them).
inline bool HitchLinesDefault() {
  static const bool on = DiagLogging() || DiagFlagOn(std::getenv("BOTS_INSTR"));
  return on;
}

}  // namespace bots
