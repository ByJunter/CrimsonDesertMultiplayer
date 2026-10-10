#include "mod/game/game_clock.h"

#include <windows.h>

#include <cstring>

#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"

namespace cdmp::game {
namespace off = build_2976;

std::optional<uint64_t> client_game_clock_ms() {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // The function must be the one we read: then its globals are too.
    static const bool known = [] {
        uint8_t head[sizeof off::kClientClockHead];
        return safe_read(base + off::kClientClock, head, sizeof head) &&
               std::memcmp(head, off::kClientClockHead, sizeof head) == 0;
    }();
    if (!known) return std::nullopt;
    // exe+0x1416B80 on a client thread: [ms] + [us] / 1000 while [ms] != 0.
    const auto ms = read<uint64_t>(base + off::kClientFrameClockMs);
    const auto us = read<uint64_t>(base + off::kClientFrameClockUs);
    const auto bias = read<int64_t>(base + off::kClientClockBias);
    if (!ms || *ms == 0 || !us || !bias) return std::nullopt;
    return static_cast<uint64_t>(static_cast<int64_t>(*ms + *us / 1000) + *bias);
}

}  // namespace cdmp::game
