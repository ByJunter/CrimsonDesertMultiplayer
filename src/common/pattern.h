#pragma once
// Byte patterns ("AOB" signatures) in Cheat Engine style: hex bytes separated
// by spaces, with "?" or "??" as wildcards, e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0".

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cdmp {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;  // true = byte must match, false = wildcard

    size_t size() const { return bytes.size(); }
};

// Returns nullopt for empty patterns, bad tokens, or all-wildcard patterns.
std::optional<Pattern> parse_pattern(std::string_view text);

// Appends the offsets of matches in `data` to `out`, stopping once `out`
// holds `max_matches` entries.
void find_pattern(std::span<const uint8_t> data, const Pattern& pattern,
                  std::vector<size_t>& out, size_t max_matches);

}  // namespace cdmp
