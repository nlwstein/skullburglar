// Test instrumentation: a machine-readable status stream and a control channel, for unattended tests, benchmarks and
// external agents (src/instr.cpp has the full description, protocol and status schema).
//
//   BOTS_INSTR=1 | <port>    on; TCP control channel on 127.0.0.1:47100 (or <port>). Off (unset): nothing runs and
//                            every hook is one predictable branch on a static bool.
//   BOTS_INSTR_BIND=<addr>   listen address (default 127.0.0.1; 0.0.0.0 to accept other machines)
//   BOTS_INSTR_LOG=<path>    also append each status line (and events) to this JSONL file
//   BOTS_INSTR_HZ=<n>        status lines per second (default 2)
//   BOTS_INSTR_WORLD=1       include the nearest pawns (enemies) in every status line (a pawn-list walk, 4 Hz)
//   BOTS_INSTR_CPU=<secs>    per-thread CPU sample interval in the status (default 5, 0 off)
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

struct PPCContext;

namespace rex::input {
class InputDriver;
}
namespace rex::system {
class IGraphicsSystem;
}

namespace bots::instr {

// BOTS_INSTR is set. Read once.
bool Enabled();

// Called once from OnPostSetup when Enabled(): starts the status and control threads and the log watcher.
void Start(std::function<rex::system::IGraphicsSystem*()> graphics);

// The control channel's pad (guest user 0, merged with real pads like the scripted one).
std::unique_ptr<rex::input::InputDriver> MakeInputDriver();

// Game thread, once per frame (the per-frame input poll, user 0), with that call's guest context: collision traces
// (line of sight, path_to) call the game's own trace code with it, and restore it.
void GameTick(PPCContext& ctx, uint8_t* base);
// Game thread: the gameplay HUD ticked (C1HUD execTickCashAndScore).
void HudTick();
// Game thread: the pause menu opened / closed (C1HUD execNativePauseGame / execNativeUnpauseGame).
void SetPaused(bool paused);
// Game thread: GameEngine::Browse (map loads) begins / ends; SetClientTravel requested `url`.
void BrowseBegin(const std::string& url);
void BrowseEnd();
void Travel(const std::string& url);
// Game thread: console commands queued by the control channel ("exec ..."), appended to `out`.
void TakeCommands(std::deque<std::string>& out);
// A console command is about to run (from any source): tracks LoadChapter for the status.
void NoteCommand(const std::string& cmd);

}  // namespace bots::instr
