// Co-op on one PC (B-057): each instance is player one in its own window, fed by one chosen pad.
// The second instance is a separate process that joins the first over system link (SDK patch 0018,
// netplay_* cvars). --coop starts both from one launch: this process is player 1 and spawns
// player 2 (SpawnPlayerTwo).
#pragma once

#include <filesystem>
#include <memory>

#include <rex/input/device_assignment.h>

namespace bots {

// --pad=N (1-4): only physical pad N (connection order) drives this instance's player; keyboard and
// mouse still follow window focus. 0 (default): every pad maps to its own player, as before.
int CoopPad();

// The assignment for --pad, or null when --pad is 0.
std::unique_ptr<rex::input::DeviceAssignment> MakePadAssignment();

// --coop: 1 or 2 for the two instances of a co-op launch, 0 otherwise.
int CoopPlayer();

// Before the window and kernel exist: with --coop, Live sign-in (Join COOP is Live-only), windowed
// on the left / right half of the screen, the player's pad (coop_pads), and player 2's own XUID
// and gamertag. Explicit command-line values win. Runs again when the setup window's Play turns
// co-op on (the window and its placement are settled by then; the rest still applies).
void ApplyCoopDefaults();

// Player 1 of --coop: start player 2 (same executable and game files; user data, cache and log
// next to player 1's with a "-p2" suffix, the cache seeded from player 1's shader cache).
void SpawnPlayerTwo(const std::filesystem::path& game_data_root,
                    const std::filesystem::path& user_data_root,
                    const std::filesystem::path& cache_root);

}  // namespace bots
