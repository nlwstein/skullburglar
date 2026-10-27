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

// Console commands ("scenefeed ..."); returns false for anything else.
bool Command(uint8_t* base, const std::string& cmd);

}  // namespace bots::scenefeed
