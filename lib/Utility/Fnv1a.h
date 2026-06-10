#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// FNV-1a hash, sized to size_t on the target. The firmware runs on a 32-bit
// RISC-V where size_t is 32 bits; the desktop simulator is typically 64-bit.
// Using the canonical constants for each width keeps the algorithm well-defined
// on both, and avoids paying for 64-bit multiplies on the MCU.
//
// IMPORTANT: this hash is suitable for in-memory lookups only (cache keys,
// dedup, hash tables). Values produced on a 32-bit target differ from those on
// a 64-bit target for the same input, so do NOT persist them to disk or any
// format that crosses build boundaries; use an explicit fixed-width hash for
// that case.
namespace Fnv1a {

static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "FNV-1a constants are only defined for 32- or 64-bit size_t");

constexpr size_t kOffsetBasis =
    sizeof(size_t) == 8 ? static_cast<size_t>(14695981039346656037ULL) : static_cast<size_t>(2166136261U);
constexpr size_t kPrime = sizeof(size_t) == 8 ? static_cast<size_t>(1099511628211ULL) : static_cast<size_t>(16777619U);

// Per-byte mix step. Pass kOffsetBasis (or a continuation seed) as the initial
// state; callers may transform the byte (e.g. asciiToLower) before mixing.
constexpr size_t mix(size_t state, unsigned char byte) { return (state ^ byte) * kPrime; }

// Hash a contiguous buffer. Pass a non-default seed to continue an in-progress
// hash across multiple buffers.
inline size_t hash(const void* data, size_t len, size_t seed = kOffsetBasis) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  size_t state = seed;
  for (size_t i = 0; i < len; ++i) state = mix(state, bytes[i]);
  return state;
}

inline size_t hash(std::string_view sv, size_t seed = kOffsetBasis) { return hash(sv.data(), sv.size(), seed); }

}  // namespace Fnv1a
