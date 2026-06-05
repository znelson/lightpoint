#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "minibidi.h"
}

namespace BidiUtils {

// Paragraph-level P2/P3: scan the first N strong chars per word to find base direction.
inline constexpr int RTL_PARAGRAPH_PROBE_DEPTH = 5;

bool startsWithRtl(const char* utf8, int maxStrongChars = RTL_PARAGRAPH_PROBE_DEPTH);

int detectParagraphLevel(const char* utf8, int fallbackLevel = 0, int maxStrongChars = 64);

// paragraphLevel: -1 = auto-detect, 0 = LTR, 1 = RTL
bool applyBidiVisual(const char* utf8, std::string& out, int paragraphLevel = -1);

bool computeVisualWordOrder(const std::vector<std::string>& words, bool paragraphIsRtl,
                            std::vector<uint16_t>& visualOrder);

// Apply a permutation in place: data[i] := original data[order[i]].
// O(n) via cycle decomposition. Precondition: order.size() <= BIDI_MAX_LINE
// (the stack visited-bitset is sized to that cap).
template <typename T>
void applyOrderInPlace(std::vector<T>& data, const std::vector<uint16_t>& order) {
  constexpr size_t kVisitedWords = (BIDI_MAX_LINE + 63) / 64;
  uint64_t visited[kVisitedWords] = {};
  for (size_t start = 0; start < order.size(); start++) {
    if ((visited[start >> 6] >> (start & 63)) & 1ULL) continue;
    T saved = data[start];
    size_t cur = start;
    while (true) {
      visited[cur >> 6] |= (1ULL << (cur & 63));
      const size_t src = order[cur];
      if (src == start) {
        data[cur] = saved;
        break;
      }
      data[cur] = data[src];
      cur = src;
    }
  }
}

}  // namespace BidiUtils
