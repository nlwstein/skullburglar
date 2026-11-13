// Scene feed (research spike): a per-frame description of what UE3 is drawing, read from the live engine objects
// through their reflection data, written as one JSON file per frame. See scene_feed.cpp for the format.
#pragma once

#include <cstdint>
#include <string>

namespace bots::scenefeed {

// Game thread, from the per-frame input poll. Does nothing unless a scenefeed command asked for work.
void Tick(uint8_t* base);

// Game thread: with hybrid fill 4, publishes the frame's meshes, particles and lights for the n1 renderer. Called from
// BeginRenderingViewFamily right after the frame's camera is published (LiveAtDraw), or else from the input poll.
void Live(uint8_t* base);

// Whether Live runs at BeginRenderingViewFamily (BOTS_N1_FEED_AT_DRAW, default 1) rather than at the next input poll.
// At the poll, the render thread could reach the frame's marker before its scene existed (stale or doubled frames).
bool LiveAtDraw();

// B-323: game thread, once per scene view family in n1 mode. Reads the player's post chain and notes whether a stylized
// counter-kill (a MerkYouSpace post material) is on; StylizedKill() returns the last result, StylizedKillToGame()
// whether that frame goes to the game's renderer (B-345: BOTS_N1_COUNTERKILL, counterkill_port.h). Nothing with the
// switch off.
void UpdateStylizedKill(uint8_t* base);
bool StylizedKill();
bool StylizedKillToGame();
bool StylizedKillPortOn();

// Console commands ("scenefeed ..."); returns false for anything else.
bool Command(uint8_t* base, const std::string& cmd);

}  // namespace bots::scenefeed
