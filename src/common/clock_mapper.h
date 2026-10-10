#pragma once
// Converts the other game's event timestamps to this game's clock. Each
// event gives a sample (our game clock on arrival - its timestamp) = clock
// difference + transit time. The minimum over a few seconds is the clock
// difference plus the fastest transit; co-op then plays events a fixed delay
// after that, so jitter only eats into the delay.
//
// The applied offset follows that minimum slowly (kSlewPerSample ms per
// event): the game clocks advance with game time (frame hitches, menus), so
// the minimum wanders by tens of ms, and every backward step of the applied
// offset makes the receiving client drop samples whose timestamps no longer
// increase. Big differences (first estimate, a paused game) are taken at once.
// After a silence (the other game sat in a menu, so it sent nothing) a sample
// that is off by more than the jump threshold either way starts a new
// estimate at once (pausa-copia-congelada.md F4).

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace cdmp {

class ClockMapper {
public:
    static constexpr uint32_t kWindowMs = 4000;
    static constexpr int64_t kSlewPerSample = 1;  // ~40 ms/s at the Move rate
    static constexpr int64_t kJumpThreshold = 250;
    static constexpr int kLateRunToReset = 20;  // ~0.5 s of Moves
    static constexpr uint32_t kSilenceMs = 1000;
    static constexpr size_t kMaxSamples = 1024;  // ~250/s in a 4 s window; a flood cannot grow it

    void add(uint64_t local_clock_ms, uint64_t remote_ts_ms, uint32_t now_ms) {
        const int64_t sample =
            static_cast<int64_t>(local_clock_ms) - static_cast<int64_t>(remote_ts_ms);
        // The other game's clock stopped (menu, loading) and resumed: every
        // sample is now far above the offset. Don't wait for the window.
        late_run_ = offset_ && sample - *offset_ > kJumpThreshold ? late_run_ + 1 : 0;
        const bool after_silence = offset_ && !samples_.empty() && now_ms - last_ms_ > kSilenceMs &&
                                   std::abs(sample - *offset_) > kJumpThreshold;
        last_ms_ = now_ms;
        if (late_run_ >= kLateRunToReset || after_silence) {
            samples_.clear();
            offset_.reset();
            late_run_ = 0;
        }
        samples_.emplace_back(now_ms, sample);
        while (now_ms - samples_.front().first > kWindowMs || samples_.size() > kMaxSamples) samples_.pop_front();
        int64_t best = samples_.front().second;
        for (const auto& s : samples_) best = std::min(best, s.second);
        target_ = best;
        if (!offset_ || std::abs(best - *offset_) > kJumpThreshold) {
            offset_ = best;
        } else {
            offset_ = *offset_ + std::clamp(best - *offset_, -kSlewPerSample, kSlewPerSample);
        }
    }

    // local clock = remote timestamp + offset (fastest transit included).
    std::optional<int64_t> offset() const { return offset_; }
    // The raw windowed minimum the offset is moving towards.
    std::optional<int64_t> target() const { return target_; }

    void reset() {
        samples_.clear();
        offset_.reset();
        target_.reset();
        late_run_ = 0;
    }

private:
    std::deque<std::pair<uint32_t, int64_t>> samples_;  // (arrival, sample)
    std::optional<int64_t> offset_;
    std::optional<int64_t> target_;
    int late_run_ = 0;
    uint32_t last_ms_ = 0;  // arrival of the latest sample
};

}  // namespace cdmp
