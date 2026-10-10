#pragma once
// Timestamps of the movement we inject into this game's client, checked
// against the client's own clock right before delivery
// (pausa-copia-congelada.md F1).
//
// The client keeps a short buffer of positions (and one of rotations) per
// remote actor, plays it 200 ms behind its "now", drops a sample whose
// timestamp is not above the last one and never extrapolates
// (movimiento-remoto-cliente.md section 2). After a pause on either side our
// clock mapping can be seconds off: samples far ahead filled the buffer and
// then every newer one was dropped, so the copy ran in place. Here each
// sample is kept within [now - kBehindMs, now + kAheadMs] and strictly
// increasing per buffer; in normal play (samples up to ~250 ms ahead with a
// 300 ms playout delay and a 100 ms hold) nothing changes. A Move that is not
// newer than the last position by a little (a repeated timestamp) is dropped
// rather than squeezed in 1 ms after it (saltos-copia-anfitrion.md section 6).

#include <algorithm>
#include <cstdint>
#include <map>

namespace cdmp {

class InjectClock {
public:
    static constexpr uint64_t kBehindMs = 500;
    static constexpr uint64_t kAheadMs = 450;
    static constexpr uint64_t kDropBehindMs = 100;  // a Move this close behind the last: dropped

    enum class Buffer { Position, Rotation, Both };  // Move, Rotate, StartAction

    // The timestamp to deliver with; 0 = drop this sample.
    uint64_t stamp(uint32_t key, Buffer b, uint64_t ts, uint64_t now) {
        const uint64_t lo = now > kBehindMs ? now - kBehindMs : 0;
        ts = std::clamp(ts, lo, now + kAheadMs);
        Last& l = last_[key];
        uint64_t floor = 0;
        if (b != Buffer::Rotation) floor = std::max(floor, l.pos);
        if (b != Buffer::Position) floor = std::max(floor, l.rot);
        if (floor && ts <= floor) {
            // Actions (StartAction, Rotate) go in when the buffer reaches them,
            // after the Moves around them: their sample is older than the
            // buffer and the client skips it but still starts the action. Pulled
            // forward it would put an old position at the newest time.
            if (b != Buffer::Position) return ts;
            if (floor - ts < kDropBehindMs) return 0;
            ts = floor + 1;
        }
        if (b != Buffer::Rotation) l.pos = ts;
        if (b != Buffer::Position) l.rot = ts;
        return ts;
    }
    // The actor was removed: a new one with this key starts a new buffer.
    void forget(uint32_t key) { last_.erase(key); }
    void clear() { last_.clear(); }

private:
    struct Last {
        uint64_t pos = 0, rot = 0;
    };
    std::map<uint32_t, Last> last_;
};

// Tells whether this game is paused (a menu stops its clock). A long frame
// or the clock stepping back when ServerTimeInfoAck corrects it also make
// the clock lag the wall for ~100-500 ms, and holding movement then made the
// copy stutter (saltos-copia-anfitrion.md section 6): paused only when the
// clock advanced less than a quarter of the wall time over >= kPauseWindowMs;
// running again as soon as it advances at least half the wall time over
// kResumeWindowMs. A step back just restarts the measurement.
class PauseDetector {
public:
    static constexpr uint64_t kPauseWindowMs = 600;
    static constexpr uint64_t kResumeWindowMs = 150;

    bool update(uint64_t game_ms, uint64_t wall_ms) {
        if (!wall_at_ || game_ms < game_at_) {
            game_at_ = game_ms;
            wall_at_ = wall_ms;
            return paused_;
        }
        const uint64_t dwall = wall_ms - wall_at_;
        const uint64_t dgame = game_ms - game_at_;
        if (paused_) {
            if (dwall < kResumeWindowMs) return paused_;
            if (dgame * 2 >= dwall) paused_ = false;
        } else {
            if (dwall < kPauseWindowMs) return paused_;
            if (dgame * 4 < dwall) paused_ = true;
        }
        game_at_ = game_ms;
        wall_at_ = wall_ms;
        return paused_;
    }
    bool paused() const { return paused_; }

private:
    uint64_t game_at_ = 0, wall_at_ = 0;
    bool paused_ = false;
};

}  // namespace cdmp
