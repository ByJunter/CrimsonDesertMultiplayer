#pragma once
// Phase 5 (fase5-suprimir-enemigos-invitado.md §4): on the GUEST, while it is
// within 150 m of the host, its own hostile enemies there are hidden and the
// host's copies stand in for them; beyond 250 m they come back. This is the
// guest's zone state machine, with no game or network in it (the tests drive
// it): the caller feeds positions and link state every tick and acts on what
// each step says (native_suppress.cpp).
//
//   Off --(within on_m for 2 s, linked, in the world, the host's copy here)--> Arming
//   Arming --(2 s)--> Asking (EnemyZone{on}) or, while one of our natives
//     fought us in the last 10 s, WaitFight first (60 s at most)
//   Asking --(the host confirms that seq)--> On: hide and sweep
//   On --(beyond off_m for 5 s | the host's copy gone 3 s | the host jumped
//     100 m in 1 s beyond off_m | the link went | the host stopped)--> Off,
//     giving our enemies back; (we jumped | loading screen | out of the
//     world) --> Off with nothing to give back (the server already removed them).
//   On, the host died and reappeared (a jump within 30 s of a dead host): the
//     zone is kept for respawn_grace (nothing given back, no Far/HostGone);
//     the host back within off_m ends the grace (zona-tras-muerte-anfitrion.md).
// Nothing is hidden before the host confirms: a host that does not relay
// would leave the guest with empty camps.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>

#include "common/protocol.h"

namespace cdmp {

class EnemyZoneMachine {
public:
    static constexpr uint64_t kOnAfterMs = 2000;    // within on_m that long
    static constexpr uint64_t kOffAfterMs = 5000;   // beyond off_m that long
    static constexpr uint64_t kHostGoneMs = 3000;   // the host's copy missing that long
    static constexpr uint64_t kCalmMs = 10000;      // no native fought us that long
    static constexpr uint64_t kAskAgainMs = 10000;  // EnemyZone{on} again while unanswered
    static constexpr uint64_t kRefusedPauseMs = 30000;  // after the host said it does not relay
    static constexpr float kJumpM = 100.0f;         // a jump: this far...
    static constexpr uint64_t kJumpWindowMs = 1000; // ...within this
    static constexpr uint64_t kJumpConfirmMs = 300; // and still there after this (not a bad read)
    static constexpr uint64_t kRespawnWindowMs = 30000;  // a host jump this soon after it was dead: a respawn

    struct Settings {
        float on_m = 150.0f;
        float off_m = 250.0f;
        uint64_t wait_fight_ms = 60000;
        uint64_t respawn_grace_ms = 60000;  // 0: a respawn jump turns the zone off as any jump
    };
    enum class State : uint8_t { Off, Arming, WaitFight, Asking, On };
    enum class Why : uint8_t {
        None, Far, LinkLost, HostGone, HostJumped, YouJumped, LoadingScreen, NotInWorld, HostStopped, Stopped
    };
    struct Input {
        uint64_t now_ms = 0;
        bool linked = false;    // connected and the game ready
        bool in_world = false;  // our player is in the world
        std::optional<proto::Vec3> host, me;  // host: its copy here, in shared coordinates
        bool host_confirmed = false;  // the host answered our current seq: it relays
        bool host_refused = false;    // the host answered our current seq: it does not relay
        uint64_t last_fight_ms = 0;   // last time one of our natives fought us (0 = never)
        bool loading_screen = false;  // the server removed everything (reason 19)
        bool host_dead = false;       // the host's PlayerState says 0 HP
    };
    struct Step {
        bool ask = false;       // send EnemyZone{on, seq()}
        bool withdraw = false;  // send EnemyZone{off, seq()}
        bool turned_on = false;
        bool turned_off = false;  // from On
        bool restore = false;     // with turned_off: give our natives back
        bool paused = false;      // the host died and reappeared far: the zone is kept (grace)
        bool resumed = false;     // the host came back within off_m during the grace
        Why why = Why::None;
    };

    explicit EnemyZoneMachine(Settings s = {}) : s_(s) {}

    Step tick(const Input& in) {
        Step st;
        const bool ok = in.linked && in.in_world && in.host && in.me;
        const float d = ok ? flat(*in.host, *in.me) : NAN;
        if (in.host) host_seen_ms_ = in.now_ms;
        if (in.host_dead) host_dead_ms_ = in.now_ms;
        last_distance_ = d;
        const Why jumped = jumps(in);
        const bool respawn = host_dead_ms_ && in.now_ms - host_dead_ms_ <= kRespawnWindowMs;
        const bool in_grace = state_ == State::On && in.now_ms < grace_until_ms_;
        if (state_ == State::Off) {
            if (ok && d <= s_.on_m && in.now_ms >= not_before_ms_) enter(State::Arming, in.now_ms);
            return st;
        }
        // Reasons to stop, whatever the state; only On has hidden anything.
        Why why = Why::None;
        bool restore = true;
        if (in.loading_screen) why = Why::LoadingScreen, restore = false;
        else if (!in.linked) why = Why::LinkLost;
        else if (!in.in_world) why = Why::NotInWorld, restore = false;
        else if (jumped == Why::HostJumped && state_ == State::On &&
                 ((respawn && s_.respawn_grace_ms) || (ok && d <= s_.off_m))) {
            // The host died and reappeared, or jumped but stays near: the zone
            // is kept (nothing given back).
            if (respawn && s_.respawn_grace_ms && !(ok && d <= s_.off_m)) {
                grace_until_ms_ = in.now_ms + s_.respawn_grace_ms;
                st.paused = true;
            }
        } else if (jumped != Why::None) why = jumped, restore = jumped != Why::YouJumped;
        else if (!in.host && in.now_ms - host_seen_ms_ > kHostGoneMs && !in_grace) why = Why::HostGone;
        else if (state_ >= State::Asking && in.host_refused) why = Why::HostStopped;
        if (ok && d <= s_.off_m) {
            far_since_ms_ = 0;
            if (grace_until_ms_) {
                grace_until_ms_ = 0;
                st.resumed = true;
            }
        } else if (ok && !far_since_ms_) {
            far_since_ms_ = in.now_ms;
        }
        // Far (with the give-back) once the grace is over.
        if (why == Why::None && far_since_ms_ && in.now_ms - far_since_ms_ >= kOffAfterMs &&
            in.now_ms >= grace_until_ms_) {
            why = Why::Far;
        }
        if (why != Why::None) return off(st, why, restore, in.now_ms);

        const bool fighting = in.last_fight_ms && in.now_ms - in.last_fight_ms < kCalmMs;
        switch (state_) {
            case State::Arming:
                if (!ok || d > s_.on_m) {
                    enter(State::Off, in.now_ms);
                } else if (in.now_ms - since_ms_ >= kOnAfterMs) {
                    if (fighting) enter(State::WaitFight, in.now_ms);
                    else ask(st, in.now_ms);
                }
                break;
            case State::WaitFight:
                if (!fighting || in.now_ms - since_ms_ >= s_.wait_fight_ms) ask(st, in.now_ms);
                break;
            case State::Asking:
                if (in.host_confirmed) {
                    enter(State::On, in.now_ms);
                    st.turned_on = true;
                } else if (in.now_ms - asked_ms_ >= kAskAgainMs) {
                    asked_ms_ = in.now_ms;
                    st.ask = true;
                }
                break;
            case State::On:
            case State::Off: break;
        }
        return st;
    }

    // Turned off from outside (the breaker, the user, a new partner).
    Step stop(Why why, bool restore, uint64_t now_ms) {
        Step st;
        if (state_ == State::Off) return st;
        return off(st, why, restore, now_ms);
    }

    State state() const { return state_; }
    bool on() const { return state_ == State::On; }
    uint32_t seq() const { return seq_; }
    float last_distance() const { return last_distance_; }
    uint64_t grace_until() const { return grace_until_ms_; }  // 0: no grace
    const Settings& settings() const { return s_; }

    static float flat(const proto::Vec3& a, const proto::Vec3& b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
    }

private:
    struct Track {
        proto::Vec3 at{};
        uint64_t ms = 0;
        bool has = false;
        proto::Vec3 from{};       // where a jump started
        uint64_t jump_ms = 0;     // when it was seen (0 = none pending)
    };
    // A position that moved kJumpM within kJumpWindowMs, and is still that far
    // from where it was kJumpConfirmMs later (one bad read is not a jump).
    static bool jumped(Track& t, const std::optional<proto::Vec3>& p, uint64_t now) {
        if (!p) return false;
        bool confirmed = false;
        if (t.jump_ms) {
            if (flat(*p, t.from) <= kJumpM) {
                t.jump_ms = 0;  // came back: a bad read
            } else if (now - t.jump_ms >= kJumpConfirmMs) {
                t.jump_ms = 0;
                confirmed = true;
            }
        } else if (t.has && now - t.ms <= kJumpWindowMs && flat(*p, t.at) > kJumpM) {
            t.from = t.at;
            t.jump_ms = now;
            return false;  // not taken as the last good position yet
        }
        if (!t.jump_ms) {
            t.at = *p;
            t.ms = now;
            t.has = true;
        }
        return confirmed;
    }
    Why jumps(const Input& in) {
        const bool me = jumped(me_, in.me, in.now_ms);
        const bool host = jumped(host_, in.host, in.now_ms);
        return me ? Why::YouJumped : host ? Why::HostJumped : Why::None;
    }
    void enter(State s, uint64_t now) {
        state_ = s;
        since_ms_ = now;
        far_since_ms_ = 0;
        grace_until_ms_ = 0;
    }
    void ask(Step& st, uint64_t now) {
        ++seq_;
        enter(State::Asking, now);
        asked_ms_ = now;
        st.ask = true;
    }
    Step& off(Step& st, Why why, bool restore, uint64_t now) {
        // A host that does not relay is not asked again at once (every 2 s).
        if (why == Why::HostStopped) not_before_ms_ = now + kRefusedPauseMs;
        st.withdraw = state_ >= State::Asking;  // the host was told
        st.turned_off = state_ == State::On;
        st.restore = st.turned_off && restore;
        st.why = why;
        enter(State::Off, 0);
        return st;
    }

    Settings s_;
    State state_ = State::Off;
    uint32_t seq_ = 0;
    uint64_t since_ms_ = 0, far_since_ms_ = 0, asked_ms_ = 0, host_seen_ms_ = 0, not_before_ms_ = 0;
    uint64_t host_dead_ms_ = 0, grace_until_ms_ = 0;
    float last_distance_ = NAN;
    Track me_, host_;
};

// The breaker (§4.4, question P4): a spawner that keeps sending what it does
// not see. Trips when one spawn identity (enemy_load::spawn_identity) was
// dropped more than kMaxSame times in kWindowMs, or more than kMaxPerMinute
// loads were dropped in a minute; then the guest stops hiding for the session.
class SpawnBreaker {
public:
    static constexpr uint64_t kWindowMs = 120000;
    static constexpr size_t kMaxSame = 4;
    static constexpr size_t kMaxPerMinute = 60;
    static constexpr size_t kMaxIdentities = 512;

    // One more drop; true when that one trips the breaker.
    bool note(uint64_t identity, uint64_t now) {
        while (!all_.empty() && now - all_.front() >= 60000) all_.pop_front();
        all_.push_back(now);
        if (same_.size() >= kMaxIdentities) {
            std::erase_if(same_, [&](const auto& e) { return e.second.empty() || now - e.second.back() >= kWindowMs; });
            if (same_.size() >= kMaxIdentities) same_.clear();
        }
        auto& times = same_[identity];
        while (!times.empty() && now - times.front() >= kWindowMs) times.pop_front();
        times.push_back(now);
        return times.size() > kMaxSame || all_.size() > kMaxPerMinute;
    }
    size_t same_count(uint64_t identity) const {
        auto it = same_.find(identity);
        return it == same_.end() ? 0 : it->second.size();
    }
    void reset() {
        same_.clear();
        all_.clear();
    }

private:
    std::map<uint64_t, std::deque<uint64_t>> same_;
    std::deque<uint64_t> all_;
};

}  // namespace cdmp
