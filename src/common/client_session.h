#pragma once
// Client-side connection state machine. Pure logic: no sockets, no clocks.
// The caller feeds it received datagrams and the current time, and it emits
// datagrams through the send callback. This keeps it unit-testable on a
// single PC without the game or a real network.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/protocol.h"
#include "common/reliable.h"

namespace cdmp {

class ClientSession {
public:
    enum class State { Connecting, Connected };
    // Initiator says Hello until Welcomed (a guest, or anyone talking to
    // cdmp_peer). Responder waits for a Hello and Welcomes it (a host). Both
    // answer a Hello, so two initiators that know each other also connect.
    enum class Role { Initiator, Responder };

    struct Stats {
        uint64_t sent = 0;
        uint64_t received = 0;
        uint64_t rejected = 0;  // malformed or unexpected datagrams
        uint64_t remote_states = 0;
        float rtt_ms = 0.0f;  // smoothed; 0 until the first Pong
        uint64_t events_sent = 0;
        uint64_t events_received = 0;
        uint64_t reliable_received = 0;
        uint64_t unknown_kinds = 0;  // events / reliable messages of a newer version, dropped
    };

    static constexpr uint32_t kHelloRetryMs = 1000;
    static constexpr uint32_t kPingIntervalMs = 1000;
    static constexpr uint32_t kTimeoutMs = 5000;

    using SendFn = std::function<void(const std::vector<uint8_t>&)>;
    using EventFn = std::function<void(const std::string&)>;
    using GameEventFn = std::function<void(const proto::GameEvent&, uint32_t now_ms)>;
    using ReliableFn = std::function<void(const ReliableChannel::Message&, uint32_t now_ms)>;

    ClientSession(std::string player_name, uint32_t mod_build, SendFn send, EventFn on_event = {},
                  Role role = Role::Initiator);

    // Drives retries, pings and timeouts. Call regularly (every few ms).
    void tick(uint32_t now_ms);

    // Sends the local player's state if connected. Rate limiting is the
    // caller's job.
    void send_local_state(const proto::PlayerState& state, uint32_t now_ms);

    // Co-op game data (only while connected; dropped otherwise).
    void send_event(proto::GameEvent event, uint32_t now_ms);
    bool send_reliable(proto::RelKind kind, const std::vector<uint8_t>& bytes, uint32_t now_ms);
    void set_game_handlers(GameEventFn on_event, ReliableFn on_reliable) {
        game_event_fn_ = std::move(on_event);
        reliable_fn_ = std::move(on_reliable);
    }
    size_t reliable_pending() const { return reliable_.unacked_fragments(); }

    // Sends Bye (best effort) and resets to Connecting.
    void disconnect(uint32_t now_ms);

    void on_datagram(std::span<const uint8_t> bytes, uint32_t now_ms);

    State state() const { return state_; }
    const Stats& stats() const { return stats_; }
    const std::string& peer_name() const { return peer_name_; }
    // Changes every time a (new) connection starts, including when the other
    // side restarted without us noticing a timeout: co-op re-sends its data.
    uint32_t epoch() const { return epoch_; }
    Role role() const { return role_; }
    const std::optional<proto::PlayerState>& remote_state() const { return remote_state_; }
    // Why we cannot connect or what is wrong with the peer's build ("" if
    // nothing): another protocol version, or another mod build.
    const std::string& peer_problem() const { return peer_problem_; }
    size_t reliable_refused() const { return reliable_.refused(); }

private:
    void send(proto::Body body, uint32_t now_ms);
    void event(const std::string& text) const;
    void reset_to_connecting(uint32_t now_ms);
    bool other_version(const proto::Packet& packet, uint32_t now_ms);
    void check_build(const std::string& name, uint32_t build);

    std::string player_name_;
    uint32_t mod_build_;
    Role role_;
    uint32_t epoch_ = 0;
    uint32_t nonce_;           // ours, random per session object
    uint32_t peer_nonce_ = 0;  // the other side's, once known
    SendFn send_fn_;
    EventFn event_fn_;
    GameEventFn game_event_fn_;
    ReliableFn reliable_fn_;
    ReliableChannel reliable_;
    uint32_t now_for_reliable_ = 0;

    State state_ = State::Connecting;
    Stats stats_;
    std::string peer_name_;
    std::string peer_problem_;
    uint32_t last_problem_event_ms_ = 0;
    // When the peer last restarted the session (a new nonce, a new name): at
    // most one every kMinRestartMs, so a peer cannot make us reload its
    // character in a loop (security review M1).
    uint32_t last_restart_ms_ = 0;
    bool restarted_ = false;  // last_restart_ms_ is set
    static constexpr uint32_t kMinRestartMs = 3000;
    std::optional<proto::PlayerState> remote_state_;

    uint32_t next_seq_ = 1;
    bool started_ = false;
    uint32_t last_hello_ms_ = 0;
    uint32_t last_ping_ms_ = 0;
    uint32_t last_recv_ms_ = 0;
};

}  // namespace cdmp
