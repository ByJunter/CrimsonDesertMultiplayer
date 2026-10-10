#include "mod/net/coop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <vector>

#include "common/equip_state.h"
#include "mod/game/offsets.h"
#include "mod/log.h"
#include "mod/net/enemy_relay.h"
#include "mod/net/loot_twin.h"
#include "mod/net/guest_hits.h"
#include "mod/net/local_capture.h"
#include "mod/net/native_suppress.h"
#include "mod/net/player_hits.h"
#include "mod/net/puppet_minimap.h"
#include "mod/net/remote_puppet.h"
#include "mod/net/time_sync.h"
#include "mod/net/troc_sniffer.h"

namespace cdmp::net {
namespace {

std::atomic<bool> g_ready{false};
constexpr size_t kMaxOutbox = 4096;            // ~1 s of events; more means the link is stuck
constexpr uint32_t kRespawnAfterRemoveMs = 1500;  // let the old copy fade out first
constexpr uint32_t kInWorldGraceMs = 500;        // a missing position read shorter than this is ignored
constexpr size_t kMaxReliableBacklog = 512;      // fragments (~560 KB) before enemy messages wait
constexpr uint32_t kRemoteSilenceMs = 8000;       // other player gone or loading
// A load often captures two template sets within ~100 ms; announce only the
// one that stays.
constexpr uint32_t kAnnounceSettleMs = 1500;

// Applies an OtherEquipItemAck payload (04 | key | serial | n x {item, slot})
// on top of the gear a SpawnInfo carries, so a later (re)load shows it. The
// summary keeps the removals a freshly loaded copy needs (equip_state.h).
void merge_equip(proto::SpawnInfo& info, const std::vector<uint8_t>& change) {
    if (!EquipState::plausible(change.data(), change.size())) return;
    EquipState state;
    if (!info.equip.empty()) state.apply(info.equip);
    state.apply(change);
    info.equip = state.snapshot(info.player_key);
}

bool relay_is_reliable(const proto::GameEvent& ev) {
    if (ev.payload.size() < 2) return false;
    uint16_t op = 0;
    std::memcpy(&op, ev.payload.data(), 2);
    for (const auto& r : game::build_2976::kRelayedAcks) {
        if (r.op == op) return r.reliable;
    }
    return false;
}

}  // namespace

void coop_set_ready() { g_ready = true; }

CoopLink::CoopLink(ClientSession& session, uint32_t delay_ms, float offset_m, bool map_pin)
    : session_(session), delay_ms_(delay_ms), offset_m_(offset_m), map_pin_(map_pin) {
    puppet_set_offset(offset_m_);
    session_.set_game_handlers(
        [this](const proto::GameEvent& ev, uint32_t now) { on_event(ev, now); },
        [this](const ReliableChannel::Message& m, uint32_t now) { on_reliable(m, now); });
    capture_set_sink([this](const proto::GameEvent& ev) {
        std::lock_guard lock(outbox_mutex_);
        if (outbox_.size() >= kMaxOutbox) {
            ++outbox_dropped_;
            return;
        }
        outbox_.push_back(ev);
    });
    log::info("Co-op: enabled (playout delay {} ms)", delay_ms_);
}

CoopLink::~CoopLink() {
    suppress_stop(/*restore=*/true);
    capture_set_sink({});
    session_.set_game_handlers({}, {});
    if (pin_owner_) puppet_map_pin(pin_owner_, std::nullopt);
    puppet_remove();
}

void CoopLink::update(const std::optional<proto::Vec3>& local_pos, const proto::Quat& local_rot,
                      uint32_t now_ms) {
    const bool connected = session_.state() == ClientSession::State::Connected;
    // Debounced: the position read fails for 50-120 ms now and then
    // (pendiente-robustez.md: 117 flickers in the logs).
    if (local_pos) local_seen_ms_ = now_ms ? now_ms : 1;
    local_in_world_ = local_pos.has_value() || (local_seen_ms_ && now_ms - local_seen_ms_ < kInWorldGraceMs);
    if (local_in_world_ && !held_enemy_spawns_.empty()) {
        log::info("Co-op: back in the world: loading {} held enemy copies", held_enemy_spawns_.size());
        while (!held_enemy_spawns_.empty()) {
            enemy_on_spawn(held_enemy_spawns_.front());
            held_enemy_spawns_.pop_front();
        }
    }
    if (was_connected_ && !connected) {
        log::info("Co-op: link lost; removing the other player");
        forget_partner(now_ms);
    }
    if (!was_connected_ && connected) {
        announced_generation_ = 0;  // a (new) partner needs our spawn data
        log::info("Co-op: connected to '{}'", session_.peer_name());
    }
    was_connected_ = connected;
    if (connected && (session_.peer_name() != partner_ || session_.epoch() != epoch_)) {
        // Someone else is on the other side now (joined, left, replaced, or
        // restarted their game): drop the old character and introduce
        // ourselves to the new one.
        if (!partner_.empty()) {
            log::info("Co-op: other side changed from '{}' to '{}'", partner_, session_.peer_name());
            forget_partner(now_ms);
        }
        ordered_out_ = 0;  // the new partner's channel starts from scratch
        partner_ = session_.peer_name();
        // Our own copy through cdmp_peer stands where we stand and keeps
        // pushing us (it has collision): walk it beside us instead.
        const bool echo = partner_.rfind("cdmp_peer", 0) == 0;
        puppet_set_offset(echo && offset_m_ == 0.0f ? kEchoOffset : offset_m_);
        epoch_ = session_.epoch();
        announced_generation_ = 0;
    }
    if (!connected) partner_.clear();
    update_map_pin(connected, now_ms);

    // Our own events go out first: the enemy work below can take 100+ ms with
    // many copies, and everything collected before it waited that long (the
    // guest saw us ~200 ms late, lag-invitado-7oct.md); then again at the end.
    const auto send_out = [&] {
        std::vector<proto::GameEvent> out;
        {
            std::lock_guard lock(outbox_mutex_);
            out.swap(outbox_);
        }
        if (!connected || !g_ready) return;  // not linked: what was collected is dropped
        for (auto& ev : out) {
            const bool equip = ev.kind == proto::EventKind::Equip;
            if (equip || (ev.kind == proto::EventKind::Relay && relay_is_reliable(ev))) {
                // Must not get lost, and must stay in order: u32 sequence first.
                std::vector<uint8_t> b(4 + ev.payload.size());
                const uint32_t seq = ordered_out_++;
                std::memcpy(b.data(), &seq, 4);
                std::memcpy(b.data() + 4, ev.payload.data(), ev.payload.size());
                session_.send_reliable(equip ? proto::RelKind::Equip : proto::RelKind::Relay, b, now_ms);
            } else {
                session_.send_event(std::move(ev), now_ms);
            }
        }
    };
    send_out();
    const auto work_from = std::chrono::steady_clock::now();
    enemy_set_linked(connected && g_ready);
    update_enemy_zone(connected && g_ready, local_pos, now_ms);
    if (!connected || !g_ready) return;
    enemy_tick();
    loot_tick();
    // Enemy messages wait in the relay while the reliable channel is backed up
    // (a slow link with many enemies), rather than being refused by it.
    while (session_.reliable_pending() < kMaxReliableBacklog) {
        auto m = enemy_take_outgoing();
        if (!m) break;
        if (!session_.send_reliable(m->first, m->second, now_ms)) {
            log::warn("Co-op: an enemy message ({} bytes) did not fit in the reliable channel", m->second.size());
        }
    }
    for (auto& ev : enemy_host_tick()) session_.send_event(std::move(ev), now_ms);
    for (auto& ev : hits_take_events()) session_.send_event(std::move(ev), now_ms);
    for (auto& ev : guest_hits_take_events()) session_.send_event(std::move(ev), now_ms);
    if (local_in_world_) {
        if (auto ev = time_sync_tick(now_ms)) session_.send_event(std::move(*ev), now_ms);
    }
    send_out();  // what came in meanwhile
    {
        // The enemy work's share of a loop (analisis-ia2/P3 §5), every 10 s.
        static std::vector<int64_t> spent;
        static uint32_t spent_at = 0;
        spent.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                              work_from)
                            .count());
        if (now_ms - spent_at >= 10000) {
            spent_at = now_ms;
            std::sort(spent.begin(), spent.end());
            log::info("Timing: network loop's enemy/zone work, 10 s: {} loops | p50 {:.1f} p95 {:.1f} max {:.1f} ms",
                      spent.size(), spent[spent.size() / 2] / 1000.0, spent[spent.size() * 95 / 100] / 1000.0,
                      spent.back() / 1000.0);
            spent.clear();
        }
    }

    // Silence from the other player: hide their character until they're back.
    if (session_.stats().remote_states != last_remote_states_) {
        last_remote_states_ = session_.stats().remote_states;
        last_remote_ms_ = now_ms;
    }
    if (puppet_spawned() && now_ms - last_remote_ms_ > kRemoteSilenceMs) {
        log::info("Co-op: nothing from the other player for {} s; removing their character",
                  kRemoteSilenceMs / 1000);
        remove_puppet(now_ms);
        remote_generation_ = 0;
        remote_silent_ = true;
    }
    if (remote_silent_ && now_ms - last_remote_ms_ < 1000 && last_spawn_ && !remote_spawn_) {
        remote_silent_ = false;
        remote_spawn_ = last_spawn_;
        if (const auto& rs = session_.remote_state()) remote_spawn_->pos = rs->pos;
        log::info("Co-op: the other player is back; reloading their character");
    }

    // The sender dismounted: the copy would stay stuck; reload it where they are.
    if (puppet_take_reload_request() && puppet_spawned() && last_spawn_ && !remote_spawn_) {
        remove_puppet(now_ms);
        remote_spawn_ = last_spawn_;
        if (const auto& rs = session_.remote_state()) remote_spawn_->pos = rs->pos;
    }

    // Vanished copy (loading screen, area change): load it again.
    constexpr uint32_t kSpawnGraceMs = 8000, kMissingMs = 3000;
    // While the world loads our injections wait: the copy is not missing, its
    // load is held (a second load of the same keys would follow it).
    const bool loading = sniffer_world_loading();
    if (loading && puppet_spawned()) {
        spawned_at_ms_ = now_ms;
        missing_since_ms_ = 0;
    }
    if (!loading && puppet_spawned() && local_pos && now_ms - spawned_at_ms_ > kSpawnGraceMs) {
        if (puppet_position()) {
            missing_since_ms_ = 0;
        } else if (!missing_since_ms_) {
            missing_since_ms_ = now_ms;
        } else if (now_ms - missing_since_ms_ > kMissingMs && last_spawn_ && !remote_spawn_) {
            log::info("Co-op: the other player's character is gone from our world (loading screen?); "
                      "loading it again");
            puppet_forget();
            remote_spawn_ = last_spawn_;
            if (const auto& rs = session_.remote_state()) remote_spawn_->pos = rs->pos;
            missing_since_ms_ = 0;
        }
    }

    // Announce ourselves (again after every new load: equipment may differ).
    const uint32_t gen = capture_template_generation();
    if (gen != seen_generation_) {
        seen_generation_ = gen;
        seen_generation_ms_ = now_ms;
    }
    // Our horse came after that (called later, live test 6 Oct: the other
    // player saw us ride on nothing): announce again, with it.
    const uint32_t horse = capture_horse_present();
    const bool horse_new = horse && horse != announced_horse_ && gen == announced_generation_;
    if (local_pos && gen != 0 && (gen != announced_generation_ || horse_new) &&
        now_ms - seen_generation_ms_ >= kAnnounceSettleMs) {
        if (auto info = capture_spawn_info(*local_pos, local_rot)) {
            const auto bytes = proto::encode_spawn(*info);
            if (session_.send_reliable(proto::RelKind::Spawn, bytes, now_ms)) {
                if (horse_new) log::info("Co-op: our horse 0x{:08X} is here now; sending our character again", horse);
                announced_generation_ = gen;
                // Never again for this horse, even if it was not in the world by now.
                announced_horse_ = horse_new ? horse : info->horse_key;
                last_spawn_sent_ = info->generation;
                log::info("Co-op: sent our character ({} bytes, spawn {})", bytes.size(),
                          info->generation);
            }
        }
    }

    // Load the other player once we are in the world ourselves.
    if (remote_spawn_ && local_pos && !loading && static_cast<int32_t>(now_ms - spawn_not_before_ms_) >= 0) {
        if (puppet_spawned()) {
            remove_puppet(now_ms);
            return;
        }
        if (puppet_spawn(*remote_spawn_)) {
            spawned_at_ms_ = now_ms;
            missing_since_ms_ = 0;
            remote_generation_ = remote_spawn_->generation;
            log::info("Co-op: the other player is in our world (spawn {})", remote_generation_);
        }
        remote_spawn_.reset();
    }
}

void CoopLink::update_map_pin(bool connected, uint32_t now_ms) {
    if (!map_pin_ || !g_ready) return;
    // While their copy is loaded here it carries its own minimap/world-map
    // icon (puppet_minimap), and a pin on top would be a second marker. The
    // pin is for when it is not: far away, or after a loading screen, from
    // the position they report.
    std::optional<proto::Vec3> where;
    if (connected && local_in_world_ && last_remote_ms_ && now_ms - last_remote_ms_ < kRemoteSilenceMs) {
        const bool icon = puppet_minimap_offset() > 0;
        if (const auto drawn = puppet_position()) {
            if (!icon) where = drawn;
        } else if (const auto& rs = session_.remote_state()) {
            where = rs->pos;
        }
    }
    if (where) pin_wanted_ms_ = now_ms;
    if (!where) {
        // Brief gaps (a missed position read) must not make it blink.
        if (pin_owner_ && now_ms - pin_wanted_ms_ > 2000) {
            puppet_map_pin(pin_owner_, std::nullopt);
            pin_owner_ = 0;
            log::info("Co-op: map pin of the other player removed");
        }
        return;
    }
    const uint32_t owner = capture_own_player_key();
    if (!owner) return;
    if (pin_owner_ && pin_owner_ != owner) {  // another save was loaded
        puppet_map_pin(pin_owner_, std::nullopt);
        pin_owner_ = 0;
    }
    const float dx = where->x - pin_pos_.x, dz = where->z - pin_pos_.z;
    const bool moved = dx * dx + dz * dz > 3.0f * 3.0f;
    // Move it once a second at most (each move is a Remove + Create for the
    // map UI), and repeat it now and then in case a loading screen dropped it.
    if (pin_owner_ == owner && (now_ms - pin_ms_ < 1000 || (!moved && now_ms - pin_ms_ < 10000))) return;
    if (!pin_owner_) {
        log::info("Co-op: map pin of the other player at ({:.0f}, {:.0f})", where->x, where->z);
    }
    puppet_map_pin(owner, where, /*replace=*/pin_owner_ == owner);
    pin_owner_ = owner;
    pin_pos_ = *where;
    pin_ms_ = now_ms;
}

// Phase 5 (native_suppress.h). Guest: its zone around the host, ticked even
// while unlinked (that turns it off and gives its enemies back). Host: the
// answer again when it changed.
void CoopLink::update_enemy_zone(bool linked, const std::optional<proto::Vec3>& local_pos, uint32_t now_ms) {
    ZoneInput in;
    in.linked = linked;
    in.in_world = local_in_world_;
    in.echo = partner_.rfind("cdmp_peer", 0) == 0;
    in.me = local_pos;
    // Where the host really is: its copy here, without the sideways offset
    // it is drawn with (through cdmp_peer: this very spot).
    if (auto p = puppet_position()) {
        p->x -= puppet_offset_m();
        in.host = p;
    }
    if (const auto& rs = session_.remote_state()) in.host_dead = rs->health_max > 0 && rs->health <= 0.0f;
    if (auto request = suppress_tick(in); request && linked) {
        session_.send_reliable(proto::RelKind::EnemyZone, proto::encode_enemy_zone(*request), now_ms);
    }
    if (!linked) return;
    if (auto answer = enemy_zone_update()) {
        session_.send_reliable(proto::RelKind::EnemyZone, proto::encode_enemy_zone(*answer), now_ms);
    }
}

// The other side's EnemyZone: the guest's request (host) or the host's
// answer (guest). Through cdmp_peer both come back here, each to its half.
void CoopLink::on_enemy_zone(const std::vector<uint8_t>& bytes, uint32_t now_ms) {
    // A real guest asks a few times a minute at most.
    while (!zone_msgs_in_.empty() && now_ms - zone_msgs_in_.front() > 10000) zone_msgs_in_.pop_front();
    if (zone_msgs_in_.size() >= 20) {
        if (zone_refused_++ % 100 == 0) log::warn("Co-op: too many enemy zone messages; ignored ({} so far)", zone_refused_);
        return;
    }
    zone_msgs_in_.push_back(now_ms);
    const auto msg = proto::decode_enemy_zone(bytes);
    if (!msg) {
        if (zone_refused_++ % 100 == 0) log::warn("Co-op: malformed enemy zone message ({} bytes) ignored", bytes.size());
        return;
    }
    const bool host = session_.role() == ClientSession::Role::Responder;
    if (strict_roles_ && msg->answer() == host) {
        // Only a guest asks and only a host answers.
        if (zone_refused_++ % 100 == 0) log::warn("Co-op: an enemy zone message for the other role; ignored");
        return;
    }
    if (msg->answer()) {
        suppress_on_answer(*msg);
    } else {
        session_.send_reliable(proto::RelKind::EnemyZone, proto::encode_enemy_zone(enemy_on_zone_request(*msg)),
                               now_ms);
    }
}

void CoopLink::remove_puppet(uint32_t now_ms) {
    if (puppet_remove()) spawn_not_before_ms_ = now_ms + kRespawnAfterRemoveMs;
}

void CoopLink::forget_partner(uint32_t now_ms) {
    remove_puppet(now_ms);
    remote_spawn_.reset();
    last_spawn_.reset();
    remote_generation_ = 0;
    remote_silent_ = false;
    // A new partner (or the same one after a reconnection) starts its silence
    // count now, not from before the link was lost: otherwise their fresh
    // character was removed and loaded again at once (live test, 21:03:53).
    last_remote_ms_ = now_ms;
    clock_.reset();
    ordered_in_ = 0;
    ordered_held_.clear();
    held_enemy_spawns_.clear();
    zone_msgs_in_.clear();
    // Our enemies come back (phase 5); a new partner is asked anew.
    suppress_stop(/*restore=*/true);
    enemy_reset_partner();
}

void CoopLink::deliver_ordered(proto::RelKind kind, std::vector<uint8_t> payload) {
    if (kind == proto::RelKind::Equip) {
        // Keep the latest gear with the spawn data too, for (re)loads.
        if (last_spawn_) merge_equip(*last_spawn_, payload);
        if (remote_spawn_) merge_equip(*remote_spawn_, payload);
        if (puppet_spawned()) {
            puppet_event(proto::GameEvent{proto::EventKind::Equip, std::move(payload)}, 0, delay_ms_);
        }
    } else if (puppet_spawned()) {
        puppet_event(proto::GameEvent{proto::EventKind::Relay, std::move(payload)}, 0, delay_ms_);
    }
}

// Host: its guest sends its character, its gear, its hits on our enemies.
// Guest: its host also sends its enemies and their hits on us, and the time.
bool CoopLink::accepts(proto::EventKind kind) const {
    if (!strict_roles_) return true;
    const bool host = session_.role() == ClientSession::Role::Responder;
    switch (kind) {
        case proto::EventKind::EnemyAct:
        case proto::EventKind::EnemyPos:
        case proto::EventKind::PlayerHit:
        case proto::EventKind::TimeSync:
            return !host;
        case proto::EventKind::GuestHit:
            return host;
        default:
            return true;
    }
}

bool CoopLink::accepts(proto::RelKind kind) const {
    if (!strict_roles_) return true;
    const bool host = session_.role() == ClientSession::Role::Responder;
    switch (kind) {
        case proto::RelKind::EnemySpawn:
        case proto::RelKind::EnemyRemove:
        case proto::RelKind::EnemyState:
            return !host;
        default:
            return true;
    }
}

void CoopLink::on_event(const proto::GameEvent& ev, uint32_t now_ms) {
    if (!accepts(ev.kind)) {
        if (refused_by_role_++ % 500 == 0) {
            log::warn("Co-op: the other player sent something only the other role sends (kind {}); ignored "
                      "({} so far)",
                      static_cast<int>(ev.kind), refused_by_role_);
        }
        return;
    }
    ++events_in_;
    last_remote_ms_ = now_ms;
    if (ev.kind == proto::EventKind::Damage) {  // about our world, not the copy
        // Never while our natives are hidden: the host's copies stand in for them.
        if (local_in_world_ && !suppress_zone_on()) apply_remote_damage(ev.payload);
        return;
    }
    if (ev.kind == proto::EventKind::PlayerHit) {  // about us, not the copy
        if (local_in_world_) hits_on_event(ev);
        return;
    }
    if (ev.kind == proto::EventKind::TimeSync) {  // about our world
        if (local_in_world_) time_sync_on_event(ev);
        return;
    }
    if (ev.kind == proto::EventKind::GuestHit) {  // about our enemies
        if (local_in_world_) guest_hits_on_event(ev, delay_ms_);
        return;
    }
    if (ev.kind == proto::EventKind::EnemyAct || ev.kind == proto::EventKind::EnemyPos) {
        // The other player's enemies: their timestamps are on the same clock
        // as the other player's own events.
        if (const auto offset = clock_.offset()) {
            enemy_on_event(ev, *offset + static_cast<int64_t>(delay_ms_), delay_ms_);
        }
        return;
    }
    if (!puppet_spawned()) return;
    if (const uint64_t ts = event_timestamp(ev)) {
        const uint64_t local = capture_game_clock_ms();
        if (local == 0) {
            // Our own clock is learnt from our own player's messages; it
            // appears as soon as we move or turn.
            if (events_waiting_clock_++ % 400 == 0) {
                log::info("Co-op: waiting for our own game clock (move once)");
            }
            return;
        }
        clock_.add(local, ts, now_ms);
    }
    const auto offset = clock_.offset();
    if (!offset) return;
    puppet_event(ev, *offset + static_cast<int64_t>(delay_ms_), delay_ms_);
}

void CoopLink::on_reliable(const ReliableChannel::Message& msg, uint32_t now_ms) {
    if (!accepts(msg.kind)) {
        if (refused_by_role_++ % 500 == 0) {
            log::warn("Co-op: the other player sent a message only the other role sends (kind {}); ignored "
                      "({} so far)",
                      static_cast<int>(msg.kind), refused_by_role_);
        }
        return;
    }
    if (msg.kind == proto::RelKind::Spawn) {
        // A real player sends its character when it loads (twice, 1.5 s
        // apart, at a save load), not in a loop: each one reloads the copy.
        while (!spawns_in_.empty() && now_ms - spawns_in_.front() > 10000) spawns_in_.pop_front();
        if (spawns_in_.size() >= 4) {
            log::warn("Co-op: the other player's character came {} times in 10 s; ignored", spawns_in_.size() + 1);
            return;
        }
        spawns_in_.push_back(now_ms);
        auto info = proto::decode_spawn(msg.bytes);
        if (!info) {
            log::warn("Co-op: malformed spawn data ({} bytes) ignored", msg.bytes.size());
            return;
        }
        log::info("Co-op: received the other player's character ({} bytes, player 0x{:08X}, "
                  "spawn {})",
                  msg.bytes.size(), info->player_key, info->generation);
        last_spawn_ = *info;
        remote_spawn_ = std::move(*info);
        remote_silent_ = false;
    } else if (msg.kind == proto::RelKind::Equip || msg.kind == proto::RelKind::Relay) {
        if (msg.bytes.size() < 4 || msg.bytes.size() > 4 + proto::kMaxEventPayload) return;
        uint32_t seq = 0;
        std::memcpy(&seq, msg.bytes.data(), 4);
        if (seq < ordered_in_) return;  // stale
        if (ordered_held_.size() < 256) {
            ordered_held_[seq] = {msg.kind, std::vector<uint8_t>(msg.bytes.begin() + 4, msg.bytes.end())};
        }
        for (auto it = ordered_held_.find(ordered_in_); it != ordered_held_.end();
             it = ordered_held_.find(ordered_in_)) {
            auto [kind, payload] = std::move(it->second);
            ordered_held_.erase(it);
            ++ordered_in_;
            deliver_ordered(kind, std::move(payload));
        }
    } else if (msg.kind == proto::RelKind::EnemySpawn) {
        if (local_in_world_) {
            enemy_on_spawn(msg.bytes);
        } else if (held_enemy_spawns_.size() < 64) {
            held_enemy_spawns_.push_back(msg.bytes);  // loaded when we are back (it was acknowledged)
        }
    } else if (msg.kind == proto::RelKind::EnemyRemove) {
        // A removal of a copy still held: drop it from the hold too.
        for (size_t i = 0; i + 4 <= msg.bytes.size(); i += 4) {
            std::erase_if(held_enemy_spawns_, [&](const std::vector<uint8_t>& s) {
                return s.size() >= 4 && std::memcmp(s.data(), msg.bytes.data() + i, 4) == 0;
            });
        }
        enemy_on_remove(msg.bytes);
    } else if (msg.kind == proto::RelKind::EnemyState) {
        enemy_on_state(msg.bytes, delay_ms_);
    } else if (msg.kind == proto::RelKind::EnemyZone) {
        on_enemy_zone(msg.bytes, now_ms);
    } else if (msg.kind == proto::RelKind::Despawn && msg.bytes.size() >= 4) {
        uint32_t gen = 0;
        std::memcpy(&gen, msg.bytes.data(), 4);
        if (gen == remote_generation_) {
            log::info("Co-op: the other player left (spawn {})", gen);
            puppet_remove();
            remote_generation_ = 0;
        }
    }
}

std::string CoopLink::describe() const {
    const auto off = clock_.offset();
    const auto target = clock_.target();
    return std::format("co-op: other player {} | events in {} | clock offset {} (target {}) | "
                       "reliable pending {} | outbox dropped {}",
                       puppet_spawned() ? "loaded" : "not loaded", events_in_,
                       off ? std::format("{} ms", *off) : std::string("unknown"),
                       target ? std::format("{} ms", *target) : std::string("-"),
                       session_.reliable_pending(), outbox_dropped_);
}

}  // namespace cdmp::net
