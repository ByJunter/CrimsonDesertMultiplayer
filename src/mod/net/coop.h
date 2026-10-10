#pragma once
// Co-op glue between the network session and the game: sends the local
// player's game events (local_capture.h) and spawn data to the other player,
// and drives the other player's character (remote_puppet.h) from theirs.
// Lives on the network thread (run_network in mod.cpp).

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/client_session.h"
#include "common/clock_mapper.h"
#include "common/protocol.h"

namespace cdmp::net {

// Called once the sniffer (injection) is installed and the puppet is set up.
void coop_set_ready();

class CoopLink {
public:
    // offset_m: sideways offset of the other player's copy. Alone on
    // cdmp_peer (your own copy comes back) and with offset 0, kEchoOffset is
    // used instead so the copy does not keep pushing you.
    static constexpr float kEchoOffset = 2.5f;
    CoopLink(ClientSession& session, uint32_t delay_ms, float offset_m, bool map_pin);
    ~CoopLink();
    CoopLink(const CoopLink&) = delete;
    CoopLink& operator=(const CoopLink&) = delete;

    // Every network loop. `local_pos` is set while the local player is in
    // the world (nothing is spawned or announced otherwise).
    void update(const std::optional<proto::Vec3>& local_pos, const proto::Quat& local_rot,
                uint32_t now_ms);

    std::string describe() const;

    // With a real other player (Steam), each side only accepts what the other
    // role sends: the host never takes enemy copies or hits on itself from its
    // guest, the guest never takes hits on its own enemies. Off on cdmp_peer
    // (this PC talking to itself, where one game plays both roles).
    void set_strict_roles(bool strict) { strict_roles_ = strict; }

private:
    bool accepts(proto::EventKind kind) const;
    bool accepts(proto::RelKind kind) const;
    bool strict_roles_ = false;
    uint64_t refused_by_role_ = 0;
    std::deque<uint32_t> spawns_in_;  // when the other player's character came lately
    void on_event(const proto::GameEvent& ev, uint32_t now_ms);
    void on_reliable(const ReliableChannel::Message& msg, uint32_t now_ms);
    void deliver_ordered(proto::RelKind kind, std::vector<uint8_t> payload);
    void remove_puppet(uint32_t now_ms);
    void forget_partner(uint32_t now_ms);
    void update_map_pin(bool connected, uint32_t now_ms);
    void update_enemy_zone(bool linked, const std::optional<proto::Vec3>& local_pos, uint32_t now_ms);
    void on_enemy_zone(const std::vector<uint8_t>& bytes, uint32_t now_ms);
    std::deque<uint32_t> zone_msgs_in_;  // when the other side's EnemyZone messages came lately
    uint64_t zone_refused_ = 0;

    ClientSession& session_;
    uint32_t delay_ms_;
    float offset_m_;
    bool was_connected_ = false;
    bool local_in_world_ = false;     // debounced: a missing read for <500 ms does not count
    uint32_t local_seen_ms_ = 0;       // last update with our position
    std::deque<std::vector<uint8_t>> held_enemy_spawns_;  // EnemySpawns that came while we were out of the world
    std::string partner_;  // who the relay says is on the other side
    uint32_t epoch_ = 0;   // session epoch the partner state belongs to

    std::mutex outbox_mutex_;  // filled on the sniffer's writer thread
    std::vector<proto::GameEvent> outbox_;
    uint64_t outbox_dropped_ = 0;

    uint32_t announced_generation_ = 0;  // template generation we sent as Spawn
    uint32_t seen_generation_ = 0;       // latest template generation and since when
    uint32_t seen_generation_ms_ = 0;
    uint32_t last_spawn_sent_ = 0;       // its SpawnInfo.generation
    uint32_t announced_horse_ = 0;       // the horse that Spawn carried (0 = none)

    std::optional<proto::SpawnInfo> remote_spawn_;  // waiting to be loaded
    std::optional<proto::SpawnInfo> last_spawn_;    // latest received, to reload after a pause
    uint32_t remote_generation_ = 0;                // loaded one
    uint32_t spawn_not_before_ms_ = 0;
    // The relay keeps answering pings when the other player is gone, so their
    // absence shows as silence: no events and no states.
    uint32_t last_remote_ms_ = 0;
    uint64_t last_remote_states_ = 0;
    bool remote_silent_ = false;
    // The game drops remote characters on its own loading screens: watch that
    // the copy still exists and reload it when it vanished.
    uint32_t spawned_at_ms_ = 0;
    uint32_t missing_since_ms_ = 0;

    // Equipment and state relays go reliably but the channel does not keep
    // order (a resent ApplyBuff could land after its Unapply), so they carry
    // a u32 sequence number and are delivered in order.
    uint32_t ordered_out_ = 0;
    uint32_t ordered_in_ = 0;
    std::map<uint32_t, std::pair<proto::RelKind, std::vector<uint8_t>>> ordered_held_;

    // The other player as a pin on our map (puppet_map_pin).
    bool map_pin_;
    uint32_t pin_owner_ = 0;  // 0 = no pin shown
    proto::Vec3 pin_pos_{};
    uint32_t pin_ms_ = 0;
    uint32_t pin_wanted_ms_ = 0;  // last time a pin was wanted

    ClockMapper clock_;
    uint64_t events_in_ = 0, events_waiting_clock_ = 0;
};

}  // namespace cdmp::net
