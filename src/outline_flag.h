// B-277(e) (#24): the enemy outline's flag, read from a pawn's SkeletalMeshComponent.
//
// The engine draws a character with a red outline when its mesh component's `bSpecialHighlight` is set (the game
// highlights enemies the player should shoot first). The property system describes a bool as a word offset plus a
// one-bit mask; the scene feed reads that word from guest memory (already in host order) and tests the mask.
// Pure functions, tested in tests/outline_flag_test.cpp.
#pragma once

#include <cstdint>

namespace bots::outline {

// Bit 0 of HybridScene::Skel::flags / N1SkelInstance::flags: draw this instance's outline.
inline constexpr uint32_t kSkelOutline = 1;

// A bool property is set when its bit is set in the word it lives in. A property that was not found (mask 0) is
// never set, so a missing field can't turn every pawn red.
constexpr bool BoolSet(uint32_t word, uint32_t mask) { return mask != 0 && (word & mask) != 0; }

// The flags of one skeletal instance from the word holding bSpecialHighlight.
constexpr uint32_t SkelFlags(uint32_t word, uint32_t mask) { return BoolSet(word, mask) ? kSkelOutline : 0; }

}  // namespace bots::outline
