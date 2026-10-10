#pragma once
// The game's time of day, pa::FieldTime (hora-del-dia.md section 8; build
// 1.0.0.2976, static analysis checked by emulating the instructions).
//
// "Raw" time is what advances: one day is 75,600,000 raw units, and the six
// night hours (21:00-03:00) take half as many raw units as their length in
// milliseconds, so night passes twice as fast. The game computes the current
// time as raw(stored) + (now - anchor) * scale, with "now" on the server's
// clock and scale 12 by default (one day ~105 real minutes).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace cdmp {

struct FieldTime {
    uint32_t day = 0, hour = 0, minute = 0, second = 0, ms = 0;
    uint8_t weekday = 0;
    uint16_t scale = 12;
    uint64_t anchor = 0;  // server clock (ms) when the value was set

    static constexpr uint64_t kRawDay = 75'600'000;
    static constexpr size_t kMemSize = 32;   // in the game's memory (pad byte at +0x15)
    static constexpr size_t kWireSize = 31;  // serialized (no pad)

    // toRaw 0x20D3460.
    uint64_t to_raw() const {
        const uint64_t t = ((static_cast<uint64_t>(hour) * 60 + minute) * 60 + second) * 1000 + ms;
        const uint64_t base = static_cast<uint64_t>(day) * kRawDay;
        if (t < 10'800'000) return base + t / 2;
        if (t < 75'600'000) return base + t - 5'400'000;
        return base + (t + 64'800'000) / 2;
    }

    // fromRaw 0x20D3360: sets the date and time, leaves scale and anchor.
    void set_raw(uint64_t raw) {
        const uint64_t d = raw / kRawDay;
        const uint64_t r = raw - d * kRawDay;
        const uint64_t t = r < 5'400'000 ? 2 * r : r < 70'200'000 ? r + 5'400'000 : 2 * r - 64'800'000;
        day = static_cast<uint32_t>(d);
        weekday = static_cast<uint8_t>(static_cast<uint32_t>(d) % 7);
        ms = static_cast<uint32_t>(t % 1000);
        second = static_cast<uint32_t>(t / 1000 % 60);
        minute = static_cast<uint32_t>(t / 60000 % 60);
        hour = static_cast<uint32_t>(t / 3'600'000 % 24);
    }

    // GetCurrentFieldTime 0x20E55A0, as raw: the stored value carried forward to `now`.
    uint64_t raw_at(uint64_t now) const { return to_raw() + (now > anchor ? (now - anchor) * scale : 0); }

    static std::optional<FieldTime> from_memory(const uint8_t* p) {
        FieldTime f;
        std::memcpy(&f.day, p + 0x00, 4);
        std::memcpy(&f.hour, p + 0x04, 4);
        std::memcpy(&f.minute, p + 0x08, 4);
        std::memcpy(&f.second, p + 0x0C, 4);
        std::memcpy(&f.ms, p + 0x10, 4);
        f.weekday = p[0x14];
        std::memcpy(&f.scale, p + 0x16, 2);
        std::memcpy(&f.anchor, p + 0x18, 8);
        if (f.hour > 23 || f.minute > 59 || f.second > 59 || f.ms > 999 || f.scale == 0 || f.scale > 1000) {
            return std::nullopt;
        }
        return f;
    }

    void to_wire(uint8_t* out) const {
        std::memcpy(out + 0, &day, 4);
        std::memcpy(out + 4, &hour, 4);
        std::memcpy(out + 8, &minute, 4);
        std::memcpy(out + 12, &second, 4);
        std::memcpy(out + 16, &ms, 4);
        out[20] = weekday;
        std::memcpy(out + 21, &scale, 2);
        std::memcpy(out + 23, &anchor, 8);
    }
};

// The raw time to set so our time of day becomes theirs: their hour on the
// nearest day to ours, so our day count never jumps (half a day at most).
inline uint64_t nearest_raw_with_time_of(uint64_t ours, uint64_t theirs) {
    constexpr uint64_t D = FieldTime::kRawDay;
    const uint64_t tod = theirs % D;
    uint64_t target = ours / D * D + tod;
    if (target + D / 2 < ours) target += D;
    else if (target > ours + D / 2 && target >= D) target -= D;
    return target;
}

}  // namespace cdmp
