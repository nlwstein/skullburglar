// Scene feed (research spike): a per-frame description of what UE3 is drawing, read from the live engine objects
// through their reflection data, written as one JSON file per frame. See scene_feed.cpp for the format.
#pragma once

#include <cstdint>
#include <string>

namespace bots::scenefeed {

// Game thread, from the per-frame input poll. Does nothing unless a scenefeed command asked for work.
void Tick(uint8_t* base);

// Game thread, same place: with hybrid fill 4, publishes the frame's static meshes for the n1 renderer.
void Live(uint8_t* base);

// Console commands ("scenefeed ..."); returns false for anything else.
bool Command(uint8_t* base, const std::string& cmd);

}  // namespace bots::scenefeed
