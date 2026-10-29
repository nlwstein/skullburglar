// Ed25519 signature check (RFC 8032) for the launcher's self-update: the releases file carries a signature that must
// verify against the public key compiled into the build before "Update now" is offered. Verification only; the
// signing side is tools/ci/ed25519_sign.py. The arithmetic follows the public-domain TweetNaCl (field elements in
// sixteen 16-bit limbs), with SHA-512 written out here; it is checked against the RFC 8032 test vectors in
// tests/update_test.cpp. Speed doesn't matter: it runs once per check.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bots::setup {

// True when `sig` (64 bytes) is a valid signature of msg[0..len) under `public_key` (32 bytes). Also false for a
// non-canonical signature (S not below the group order) or a public key that isn't a curve point.
bool Ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t len, const uint8_t public_key[32]);

// Lowercase or uppercase hex of exactly `bytes` bytes to raw bytes; false when it isn't that.
bool HexToBytes(const std::string& hex, uint8_t* out, size_t bytes);

}  // namespace bots::setup
