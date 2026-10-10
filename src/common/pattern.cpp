#include "common/pattern.h"

#include <cstring>

namespace cdmp {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<Pattern> parse_pattern(std::string_view text) {
    Pattern p;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < text.size() && text[end] != ' ' && text[end] != '\t') ++end;
        const std::string_view token = text.substr(i, end - i);
        i = end;

        if (token == "?" || token == "??") {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            continue;
        }
        if (token.size() != 2) return std::nullopt;
        const int hi = hex_value(token[0]);
        const int lo = hex_value(token[1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p.mask.push_back(true);
    }

    bool any_fixed = false;
    for (bool m : p.mask) any_fixed |= m;
    if (!any_fixed) return std::nullopt;
    return p;
}

void find_pattern(std::span<const uint8_t> data, const Pattern& pattern,
                  std::vector<size_t>& out, size_t max_matches) {
    const size_t n = pattern.size();
    if (n == 0 || data.size() < n || out.size() >= max_matches) return;

    // Anchor on the first fixed byte and let memchr do the heavy lifting.
    size_t anchor = 0;
    while (!pattern.mask[anchor]) ++anchor;
    const uint8_t anchor_byte = pattern.bytes[anchor];

    const uint8_t* base = data.data();
    const size_t last_start = data.size() - n;
    size_t start = 0;
    while (start <= last_start) {
        const void* hit = std::memchr(base + start + anchor, anchor_byte, last_start - start + 1);
        if (!hit) return;
        const size_t candidate = static_cast<size_t>(static_cast<const uint8_t*>(hit) - base) - anchor;

        bool match = true;
        for (size_t j = 0; j < n; ++j) {
            if (pattern.mask[j] && base[candidate + j] != pattern.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            out.push_back(candidate);
            if (out.size() >= max_matches) return;
        }
        start = candidate + 1;
    }
}

}  // namespace cdmp
