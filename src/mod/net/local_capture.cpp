#include "mod/net/local_capture.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <vector>

#include <safetyhook.hpp>

#include "common/equip_state.h"
#include "mod/game/actor_lookup.h"
#include "mod/game/game_clock.h"
#include "mod/game/offsets.h"
#include "mod/game/player_reader.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/aggro_test.h"
#include "mod/net/enemy_relay.h"
#include "mod/net/attack_probe.h"
#include "mod/net/guest_hits.h"
#include "mod/net/loot_twin.h"
#include "mod/net/native_suppress.h"
#include "common/keys.h"
#include "mod/net/piece4_test.h"
#include "mod/net/player_hits.h"
#include "mod/net/remote_puppet.h"
#include "mod/net/troc_sniffer.h"

namespace cdmp::net {
namespace {
namespace off = game::build_2976;
using Bytes = std::vector<uint8_t>;

// Keys our own puppet uses (remote_puppet.cpp): never learn templates from those.
constexpr uint32_t kPuppetUser = 0x9010F001;
constexpr uint32_t kPuppetPlayer = 0xA010F001;
constexpr uint32_t kPuppetBody = 0xB010F001;
constexpr uint32_t kPuppetHorse = 0xB010F002;

constexpr size_t kMoveReqSize = 514;
constexpr size_t kMoveWireSize = 0xEA;  // what is sent of it (see emit)
constexpr size_t kStartActionReqSize = 725;
constexpr size_t kRotateReqSize = 176;
constexpr size_t kWallUpSize = 132;
constexpr size_t kMoveTs = 0xD5;
constexpr size_t kStartActionTs = 0x15F;
constexpr size_t kRotateTs = 0x9C;

uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

uint32_t u32_at(const Bytes& v, size_t off) {
    uint32_t x = 0;
    std::memcpy(&x, v.data() + off, sizeof x);
    return x;
}

bool contains_u32(const Bytes& v, uint32_t value) {
    for (size_t i = 0; i + 4 <= v.size(); ++i) {
        if (std::memcmp(v.data() + i, &value, 4) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------- templates
std::mutex g_mutex;  // templates, keys and sink (never taken on game threads)
Bytes g_user;        // payload of the user-owned player actor load
Bytes g_body;        // payload of the player's body load
uint32_t g_user_key = 0, g_player_key = 0, g_body_key = 0;
// The player's horse (docs/research/caballo-coop.md): one user-owned
// ActorLoadDataAck of kind 5, loaded once per session.
Bytes g_horse;
uint32_t g_horse_key = 0;
std::atomic<uint32_t> g_generation{0};
uint32_t g_spawn_counter = 0;
EventSink g_sink;

// ---------------------------------------------------------------- combat tests
// The client runs the AI and reports NPC movement with MoveActorReq.
struct ActorSeen {
    proto::Vec3 pos;
    uint64_t ms;
};
std::mutex g_actors_mutex;
std::map<uint32_t, ActorSeen> g_actors;
std::atomic<uint32_t> g_last_attack_hash{0};
std::atomic<uint32_t> g_last_attack_target{0};
std::set<uint32_t> g_has_hp;  // under g_actors_mutex
constexpr uint16_t kOpVaryHPAck = 2147;
constexpr uint16_t kOpNotifyHandleDeadAck = 2530;
constexpr uint16_t kOpVaryStatByAccInputReq = 2181;
constexpr uint32_t kStatHP = 0x000F4240;

// World actors with hit points, from their loads and HP messages.
struct Enemy {
    uint32_t type = 0;      // character variant id (+0x18 of an NPC load), same on every PC
    bool has_spawn = false;
    proto::Vec3 spawn{};    // where it was loaded: same in both worlds, unlike where it is now
    int64_t hp = -1;        // x1000; -1 unknown
    int64_t hp_max = -1;
    bool dead = false;
};
std::map<uint32_t, Enemy> g_enemies;  // under g_actors_mutex
// [coop] damage, off by default: guest_hits replaces it (security review H1).
std::atomic<bool> g_damage_enabled{false};
std::atomic<bool> g_key_alarm{false};
uint64_t g_damage_sent = 0, g_damage_applied = 0, g_damage_unmatched = 0;
constexpr uint16_t kOpAttackTargetAck = 2922;  // PostProcessAttackResult_AttackTargetAck
constexpr uint16_t kOpChangedEquipByQuickSlotAck = 2667;  // 6 B: u32 player | u16 weapon set

// ---------------------------------------------------------------- writer-thread state
game::PlayerReader g_reader;
bool g_reader_ready = false;
uint32_t g_own_key = 0;             // copy of g_player_key for the writer thread
uint32_t g_own_horse = 0;           // copy of g_horse_key for the writer thread
// Horse movement: it sends its own Moves only ~1/s, so while nobody rides it
// we send moves built from its transform in memory (as for the player).
Bytes g_horse_last_move;            // its last own MoveActorReq (template)
uint64_t g_ridden_until_ms = 0;     // the rider's Moves carry it until then
float g_horse_sent_pos[3] = {NAN, NAN, NAN};
uint64_t g_horse_last_synth_ms = 0;
Bytes g_last_move;                  // last real MoveActorReq payload (template for synth)
uint64_t g_last_move_ms = 0;        // steady_ms when it was seen
uint64_t g_last_move_ts = 0;        // its game timestamp (+0xD5)
float g_last_sent_pos[3] = {NAN, NAN, NAN};
uint64_t g_last_synth_ms = 0;
uint64_t g_synthesized = 0;
constexpr uint64_t kSynthAfterSilenceMs = 150;  // real MoveActorReqs come every ~25 ms
constexpr uint64_t kSynthIntervalMs = 50;
constexpr float kSynthMinMove = 0.05f;          // metres

// Game clock: last own Req timestamp and when we saw it.
std::atomic<uint64_t> g_clock_ts{0};
std::atomic<uint64_t> g_clock_at{0};
// Whether capture_game_clock_ms reads the game's own clock (game_clock.h),
// which stops in menus, instead of estimating it from the last Req and the
// wall clock, which runs on through a pause (pausa-copia-congelada.md F2).
// Trusted once it agrees with the timestamps of our first Reqs.
std::atomic<int> g_clock_reader{0};  // 0 checking, 1 trusted, -1 off
int g_clock_good = 0, g_clock_bad = 0;  // note_clock only

constexpr uint64_t kWallUpRepeatMs = 50;  // resend an unchanged path this often
Bytes g_last_wallup;
uint64_t g_last_wallup_ms = 0;
uint64_t g_wallups = 0;
uintptr_t g_exe_base = 0;

// ---------------------------------------------------------------- equipment
// The equip paths of the in-process server all end in the writer of
// OtherEquipItemAck, the message the MMO sent to OTHER players. We copy its
// arguments (server thread) and replay the same message on the puppet. Item
// keys are static game-data ids, identical on every PC.
struct EquipCopy {
    uint32_t key;
    uint32_t serial;
    uint32_t count;
    struct {
        uint32_t item;
        uint16_t slot;
    } items[32];
};
SafetyHookMid g_equip_hook;
std::atomic<uint32_t> g_equip_key{0};  // our player key, for the hook
SRWLOCK g_equip_lock = SRWLOCK_INIT;
// Hook -> writer thread. Fixed storage: the hooks run on game server
// threads and must not allocate.
EquipCopy g_equip_pending[16];
uint32_t g_equip_pending_n = 0;
std::mutex g_slots_mutex;  // g_equip_state/generation (writer thread + spawn requests)
EquipState g_equip_state;  // changes since the templates
uint32_t g_equip_generation = 0;  // template generation the state belongs to

bool copy_equip_args(uintptr_t actor, uintptr_t serial_ptr, uintptr_t list, EquipCopy& out) {
    __try {
        out.key = *reinterpret_cast<const uint32_t*>(actor + off::kServerActor_Key);
        out.serial = *reinterpret_cast<const uint32_t*>(serial_ptr);
        const auto data = *reinterpret_cast<const uintptr_t*>(list);
        out.count = *reinterpret_cast<const uint32_t*>(list + 8);
        if (out.count > 32 || (out.count && !data)) return false;
        for (uint32_t i = 0; i < out.count; ++i) {
            out.items[i].item = *reinterpret_cast<const uint32_t*>(data + i * 8);
            out.items[i].slot = *reinterpret_cast<const uint16_t*>(data + i * 8 + 4);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void on_equip_writer(safetyhook::Context& ctx) {
    const uint32_t own = g_equip_key.load(std::memory_order_relaxed);
    if (!own) return;
    EquipCopy c{};
    if (!copy_equip_args(ctx.rcx, ctx.r8, ctx.r9, c) || c.key != own) return;
    // Never stall the server thread; the writer holds this lock only to copy.
    for (int spin = 0; !TryAcquireSRWLockExclusive(&g_equip_lock); ++spin) {
        if (spin > 1000) return;
        YieldProcessor();
    }
    if (g_equip_pending_n < 16) g_equip_pending[g_equip_pending_n++] = c;
    ReleaseSRWLockExclusive(&g_equip_lock);
}

// Weapon to hand / to back: the server runs a ChangeDockingSocket frame event
// and calls the writer of its relay message, which exits early without
// recipients. Copy the accessor (12 wire bytes) from its arguments.
struct DockCopy {
    uint32_t key;
    uint8_t type, slot;
    uint32_t hash;
    uint16_t id;
    uint32_t index;
};
SafetyHookMid g_dock_hook;
DockCopy g_dock_pending[16];  // under g_equip_lock
uint32_t g_dock_pending_n = 0;
uint64_t g_docks = 0;

bool copy_dock_args(uintptr_t actor, uintptr_t acc, DockCopy& out) {
    __try {
        out.key = *reinterpret_cast<const uint32_t*>(actor + off::kServerActor_Key);
        out.type = *reinterpret_cast<const uint8_t*>(acc + 0x08);
        out.slot = *reinterpret_cast<const uint8_t*>(acc + 0x10);
        out.hash = *reinterpret_cast<const uint32_t*>(acc + 0x14);
        out.id = *reinterpret_cast<const uint16_t*>(acc + 0x18);
        out.index = *reinterpret_cast<const uint32_t*>(acc + 0x1C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void on_dock_writer(safetyhook::Context& ctx) {
    const uint32_t own = g_equip_key.load(std::memory_order_relaxed);
    if (!own) return;
    DockCopy c{};
    // Field widths are proven for type 0 (action) only: the accessor is a
    // union by type.
    if (!copy_dock_args(ctx.rcx, ctx.r8, c) || c.key != own || c.type != 0) return;
    for (int spin = 0; !TryAcquireSRWLockExclusive(&g_equip_lock); ++spin) {
        if (spin > 1000) return;
        YieldProcessor();
    }
    if (g_dock_pending_n < 16) g_dock_pending[g_dock_pending_n++] = c;
    ReleaseSRWLockExclusive(&g_equip_lock);
}

// OtherEquipItemAck payload: 04 | u32 key | u32 serial | n x {u32 item, u16 slot}.
Bytes equip_payload(uint32_t key, uint32_t serial, const std::vector<std::pair<uint32_t, uint16_t>>& items) {
    Bytes b(9 + 6 * items.size());
    b[0] = 4;
    std::memcpy(b.data() + 1, &key, 4);
    std::memcpy(b.data() + 5, &serial, 4);
    for (size_t i = 0; i < items.size(); ++i) {
        std::memcpy(b.data() + 9 + i * 6, &items[i].first, 4);
        std::memcpy(b.data() + 13 + i * 6, &items[i].second, 2);
    }
    return b;
}

void emit(proto::EventKind kind, Bytes payload);

void note_equip(uint32_t serial, const std::vector<std::pair<uint32_t, uint16_t>>& items) {
    {
        std::lock_guard lock(g_slots_mutex);
        const uint32_t gen = g_generation.load();
        if (gen != g_equip_generation) {  // new templates already show the current gear
            g_equip_state.clear();
            g_equip_generation = gen;
        }
        g_equip_state.apply_items(serial, items);
    }
    log::info("Capture: equipment change ({} slot(s), serial {})", items.size(), serial);
    guest_hits_on_gear_change();
    emit(proto::EventKind::Equip, equip_payload(g_own_key, serial, items));
}

void drain_equip() {
    std::vector<EquipCopy> pending;
    std::vector<DockCopy> docks;
    pending.reserve(16);
    docks.reserve(16);
    AcquireSRWLockExclusive(&g_equip_lock);
    pending.assign(g_equip_pending, g_equip_pending + g_equip_pending_n);
    docks.assign(g_dock_pending, g_dock_pending + g_dock_pending_n);
    g_equip_pending_n = g_dock_pending_n = 0;
    ReleaseSRWLockExclusive(&g_equip_lock);
    for (const auto& d : docks) {
        // ChangeDockingSocketByFrameEventAck: u32 key | u8 type | u8 slot |
        // u32 hash | u16 id | u32 index.
        Bytes b(16);
        std::memcpy(b.data(), &g_own_key, 4);
        b[4] = d.type;
        b[5] = d.slot;
        std::memcpy(b.data() + 6, &d.hash, 4);
        std::memcpy(b.data() + 10, &d.id, 2);
        std::memcpy(b.data() + 12, &d.index, 4);
        if (g_docks++ < 20) {
            log::info("Capture: weapon docking event (action 0x{:08X}, frame event {})", d.hash, d.index);
        }
        emit(proto::EventKind::Docking, std::move(b));
    }
    for (const auto& c : pending) {
        std::vector<std::pair<uint32_t, uint16_t>> items;
        for (uint32_t i = 0; i < c.count; ++i) items.emplace_back(c.items[i].item, c.items[i].slot);
        note_equip(c.serial, items);
    }
}

void emit(proto::EventKind kind, Bytes payload) {
    // MoveActorReq: the Ack uses only its first 0xEA bytes; the trailing
    // 10 x 28-byte history (280 of 514 bytes) is never needed by the receiver.
    if (kind == proto::EventKind::Move && payload.size() == kMoveReqSize) payload.resize(kMoveWireSize);
    EventSink sink;
    {
        std::lock_guard lock(g_mutex);
        sink = g_sink;
    }
    if (sink) sink(proto::GameEvent{kind, std::move(payload)});
}

void note_clock(uint64_t ts) {
    if (ts == 0) return;
    g_clock_ts.store(ts, std::memory_order_relaxed);
    g_clock_at.store(steady_ms(), std::memory_order_relaxed);
    if (g_clock_reader.load(std::memory_order_relaxed) != 0) return;
    const auto read = game::client_game_clock_ms();
    if (!read) return;
    // Read a little after the Req was stamped (this runs on the capture writer).
    const int64_t diff = static_cast<int64_t>(*read) - static_cast<int64_t>(ts);
    if (diff >= -250 && diff <= 250) ++g_clock_good;
    else if (diff < -1000 || diff > 1000) ++g_clock_bad;
    if (g_clock_good >= 50) {
        g_clock_reader = 1;
        log::info("Capture: using the game's own clock (agrees with our Reqs; last difference {} ms)", diff);
    } else if (g_clock_bad >= 50) {
        g_clock_reader = -1;
        log::warn("Capture: the game clock read disagrees with our Reqs ({} ms); estimating it instead", diff);
    }
}

// Ledge climb-up path (UpdateWallUpData). A remote character doing the
// climb-up action places its animation along its WallUpPath, which only this
// message fills; without it the copy's model vanished for the whole climb-up
// (measured live: its path stayed at (0,0,0)). The MMO server relayed the
// climber's path every frame while it was in movement method 5; the
// single-player client never sends it, so read it from memory.
// Wire layout: docs/research/broadcast-servidor-movimiento.md §10.3.
std::optional<Bytes> read_player_wallup() {
    auto cc = g_reader.character_control();
    if (!cc) return std::nullopt;
    auto mover = game::read_ptr(*cc + off::kControl_MoveController);
    if (!mover) return std::nullopt;
    auto method = game::read<uint8_t>(*mover + off::kMoveController_Method);
    if (!method || *method != off::kMoveMethodWallUp) return std::nullopt;
    auto wallup = game::read_ptr(*mover + off::kMoveController_WallUp);
    if (!wallup || !game::has_vtable(*wallup, g_exe_base + off::kVtWallUpMovementMethod))
        return std::nullopt;
    auto d = game::read_ptr(*cc + off::kControl_WallData);
    if (!d) return std::nullopt;

    Bytes out(4);
    std::memcpy(out.data(), &g_own_key, 4);
    auto put = [&](uintptr_t addr, size_t n) {
        const size_t at = out.size();
        out.resize(at + n);
        return game::safe_read(addr, out.data() + at, n);
    };
    const bool ok = put(*d + 0xEE0, 1)       // phase
                    && put(*d + 0xEE4, 4)    // height class
                    && put(*d + 0x1170, 36)  // WallUpPath _up, _forward, _end
                    && put(*d + 0x11C0, 36)  // WallPassPath _up, _forward, _end
                    && put(*d + 0x11E4, 4)   // _wallThickness
                    && put(*d + 0x11E8, 3)   // _isValid, _onlyWallPass, _canAutoWallPass
                    && put(*d + 0x1200, 4)   // navigation object id
                    && put(*d + 0xF40, 12)   // support / raycast point
                    && put(*d + 0xF4C, 12)   // -forward (wall normal)
                    && put(*d + 0x10DC, 12)  // forward (xz)
                    && put(*d + 0x1100, 4);  // wall height
    if (!ok || out.size() != kWallUpSize) return std::nullopt;
    return out;
}

void capture_wallup(uint64_t now) {
    if (!g_exe_base) g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto payload = read_player_wallup();
    if (!payload) {
        g_last_wallup.clear();
        return;
    }
    if (*payload == g_last_wallup && now - g_last_wallup_ms < kWallUpRepeatMs) return;
    const bool first = g_last_wallup.empty();
    g_last_wallup = *payload;
    g_last_wallup_ms = now;
    if (first && g_wallups++ < 50) {
        float up[3];
        std::memcpy(up, g_last_wallup.data() + 0x09, sizeof up);
        log::info("Capture: climb-up path (edge {:.2f}, {:.2f}, {:.2f}) [{} climb-ups]", up[0], up[1],
                  up[2], g_wallups);
    }
    emit(proto::EventKind::WallUp, std::move(*payload));
}

// While the game sends no MoveActorReq but the player does move (vehicles,
// some animations), send moves built from the real transform in memory.
void capture_synth(uint64_t now) {
    if (g_last_move.empty() || now - g_last_move_ms < kSynthAfterSilenceMs ||
        now - g_last_synth_ms < kSynthIntervalMs) {
        return;
    }
    auto me = g_reader.read();
    if (!me) return;
    const float dx = me->position.x - g_last_sent_pos[0];
    const float dy = me->position.y - g_last_sent_pos[1];
    const float dz = me->position.z - g_last_sent_pos[2];
    if (dx * dx + dy * dy + dz * dz < kSynthMinMove * kSynthMinMove) return;  // not moving

    // The last real MoveActorReq with the player's true transform patched in.
    Bytes req = g_last_move;
    const float pos[3] = {me->position.x, me->position.y, me->position.z};
    const float rot[4] = {me->rotation.x, me->rotation.y, me->rotation.z, me->rotation.w};
    std::memcpy(req.data() + 0x04, pos, sizeof pos);  // current position
    std::memcpy(req.data() + 0x10, rot, sizeof rot);  // facing (matches q@10, verified)
    std::memcpy(req.data() + 0x4C, pos, sizeof pos);  // second copy (equal when moving)
    std::memcpy(req.data() + 0x58, rot, sizeof rot);
    // Game clock: the game's own (stops in menus), else advanced from the
    // last real Move by the wall clock.
    uint64_t ts = g_clock_reader.load(std::memory_order_relaxed) == 1 ? capture_game_clock_ms() : 0;
    if (ts == 0) ts = g_last_move_ts + (now - g_last_move_ms);
    if (ts <= g_last_move_ts) ts = g_last_move_ts + 1;  // the frame clock can lag the last real Move
    std::memcpy(req.data() + kMoveTs, &ts, 8);
    std::memcpy(g_last_sent_pos, pos, sizeof pos);
    g_last_synth_ms = now;
    if (g_synthesized++ % 100 == 0) {
        log::info("Capture: {} moves synthesised from memory (no MoveActorReq while moving)",
                  g_synthesized);
    }
    emit(proto::EventKind::Move, std::move(req));
}

// The unridden horse: Moves built from its transform in memory every 50 ms
// while it moves (it follows the player, comes when called...).
void capture_horse_synth(uint64_t now) {
    if (!g_own_horse || g_horse_last_move.empty() || now < g_ridden_until_ms ||
        now - g_horse_last_synth_ms < kSynthIntervalMs) {
        return;
    }
    const uint64_t clock = capture_game_clock_ms();
    auto pos = game::actor_world_position(g_own_horse);
    if (!pos || clock == 0) return;
    const float dx = pos->x - g_horse_sent_pos[0], dy = pos->y - g_horse_sent_pos[1],
                dz = pos->z - g_horse_sent_pos[2];
    if (dx * dx + dy * dy + dz * dz < kSynthMinMove * kSynthMinMove) return;
    Bytes req = g_horse_last_move;
    const float p[3] = {pos->x, pos->y, pos->z};
    std::memcpy(req.data() + 0x04, p, sizeof p);
    std::memcpy(req.data() + 0x4C, p, sizeof p);
    if (auto rot = game::actor_world_rotation(g_own_horse)) {
        const float q[4] = {rot->x, rot->y, rot->z, rot->w};
        std::memcpy(req.data() + 0x10, q, sizeof q);
        std::memcpy(req.data() + 0x58, q, sizeof q);
    }
    std::memcpy(req.data() + kMoveTs, &clock, 8);
    std::memcpy(g_horse_sent_pos, p, sizeof p);
    g_horse_last_synth_ms = now;
    emit(proto::EventKind::Move, std::move(req));
}

}  // namespace

void capture_observe_s2c(const uint8_t* packet, size_t size) {
    enemy_observe_s2c(packet, size);
    suppress_observe_s2c(packet, size);
    hits_observe_s2c(packet, size);
    guest_hits_observe_s2c(packet, size);
    piece4_observe_s2c(packet, size);
    puppet_observe_s2c(packet, size);
    uint16_t op = 0;
    if (size >= 5) std::memcpy(&op, packet, 2);
    if (op == kOpChangedEquipByQuickSlotAck && size == 5 + 6) {
        // u32 key | u16 weapon set: our player switched weapons from the quick
        // slots (no OtherEquipItemAck; capture 20261006-190309): our attacks change.
        uint32_t key = 0;
        std::memcpy(&key, packet + 5, 4);
        if (key && key == g_own_key) {
            log::info("Capture: weapon set changed from the quick slots");
            guest_hits_on_gear_change();
        }
        return;
    }
    if (size < 5 + 12) return;
    if (op == off::kOpUnequipItemAck && size == 5 + 12) {
        // u32 key | u16 slot | u16 inventory slot | u32 serial: the server's
        // OtherEquipItemAck for an unequip is {0, slot}.
        uint32_t key = 0, serial = 0;
        uint16_t slot = 0;
        std::memcpy(&key, packet + 5, 4);
        std::memcpy(&slot, packet + 9, 2);
        std::memcpy(&serial, packet + 13, 4);
        if (key && key == g_equip_key.load()) note_equip(serial, {{0u, slot}});
        return;
    }
    if (op == kOpAttackTargetAck && size >= 5 + 0x28) {
        // +0 target, +4 attacker, +0x24 attack hash (combate-coop.md §1.4).
        uint32_t attacker = 0, hash = 0;
        std::memcpy(&attacker, packet + 5 + 4, 4);
        std::memcpy(&hash, packet + 5 + 0x24, 4);
        uint32_t target = 0;
        std::memcpy(&target, packet + 5, 4);
        if (attacker && attacker == g_equip_key.load()) {
            g_last_attack_hash = hash;
            g_last_attack_target = target;
            std::lock_guard lock(g_actors_mutex);
            g_has_hp.insert(target);
        }
        return;
    }
    if (op == kOpVaryHPAck && size >= 5 + 0x58) {
        // +0 key, +4 stat id, +8 i64 hp x1000, +0x18 i64 max x1000, +0x54 source.
        uint32_t key = 0, stat = 0, source = 0;
        int64_t hp = 0, hp_max = 0;
        std::memcpy(&key, packet + 5, 4);
        std::memcpy(&stat, packet + 5 + 4, 4);
        std::memcpy(&hp, packet + 5 + 8, 8);
        std::memcpy(&hp_max, packet + 5 + 0x18, 8);
        std::memcpy(&source, packet + 5 + 0x54, 4);
        if (keys::is_game_world_key(key) && stat == kStatHP) {
            std::optional<Bytes> event;
            {
                std::lock_guard lock(g_actors_mutex);
                if (g_has_hp.size() < 8192) g_has_hp.insert(key);
                Enemy& e = g_enemies[key];
                const int64_t before = e.hp >= 0 ? e.hp : hp_max;
                e.hp = hp;
                e.hp_max = hp_max;
                // Damage dealt by the local player: mirror it to the other world.
                const uint32_t own = g_equip_key.load();
                if (own && source == own && hp < before && e.type && g_damage_enabled) {
                    Bytes b(proto::kDamageEventSize);
                    // Where it spawned (same in both worlds); else where it is.
                    proto::Vec3 pos{};
                    if (e.has_spawn) pos = e.spawn;
                    else if (auto it = g_actors.find(key); it != g_actors.end()) pos = it->second.pos;
                    const int64_t delta = hp - before;
                    const uint8_t killed = hp <= 0 ? 1 : 0;
                    std::memcpy(b.data(), &e.type, 4);
                    std::memcpy(b.data() + 4, &pos.x, 4);
                    std::memcpy(b.data() + 8, &pos.y, 4);
                    std::memcpy(b.data() + 12, &pos.z, 4);
                    std::memcpy(b.data() + 16, &delta, 8);
                    b[24] = killed;
                    event = std::move(b);
                }
            }
            if (event) {
                if (g_damage_sent++ < 100) {
                    log::info("Capture: we hurt enemy 0x{:08X} (hp now {:.1f})", key, hp / 1000.0);
                }
                emit(proto::EventKind::Damage, std::move(*event));
            }
        }
    }
    if (op == kOpNotifyHandleDeadAck && size >= 5 + 4) {
        uint32_t key = 0;
        std::memcpy(&key, packet + 5, 4);
        std::lock_guard lock(g_actors_mutex);
        if (auto it = g_enemies.find(key); it != g_enemies.end()) it->second.dead = true;
    }
    for (const auto& r : off::kRelayedAcks) {
        if (r.op != op) continue;
        uint32_t subject = 0;
        std::memcpy(&subject, packet + 5, 4);
        const uint32_t own = g_equip_key.load();
        if (own && subject == own && size - 5 <= proto::kMaxEventPayload - 2) {
            Bytes b(2 + size - 5);
            std::memcpy(b.data(), &op, 2);
            std::memcpy(b.data() + 2, packet + 5, size - 5);
            emit(proto::EventKind::Relay, std::move(b));
        }
        return;
    }
    if (op != off::kOpActorLoadDataAck || size < 5 + 0x20) return;
    Bytes payload(packet + 5, packet + size);
    if (const uint32_t k = u32_at(payload, 3); keys::is_mod_key(k) && !is_enemy_copy_key(k) &&
                                               (k & 0x0FFFF000u) != 0x0010F000u) {
        // Not one of ours (enemy copies, or the other player's copy at index
        // 0xF001-0xF003) yet in our range: the game's key queue is not what
        // pendiente-rango-claves.md measured. Stop making copies.
        if (!g_key_alarm.exchange(true)) {
            log::error("Capture: the game loaded actor 0x{:08X}, a key in the mod's range: enemy copies OFF", k);
        }
    }
    {
        // NPC loads: u16 size | 03 | u32 key | ... | +0x14 owner user | +0x18
        // u32 characterinfo key | +0x1C u32 faction key (0 for animals and
        // civilians) | +0x20 u32 n + UTF-16 name
        // ("Bleed Bandit"). The spawn position is in the last Transform: scale
        // (1,1,1) + an all-FF parent, position 28 bytes before (found in 161 of
        // 167 NPC loads). Kept to pair enemies across worlds.
        const uint32_t npc = u32_at(payload, 3);
        if (payload[2] == 3 && keys::is_game_world_key(npc) && payload.size() > 0x40) {
            static const uint8_t kMarker[20] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
                                                0x80, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            Enemy e;
            e.type = u32_at(payload, 0x18);
            for (size_t i = payload.size() - sizeof kMarker; i > 28; --i) {
                if (std::memcmp(payload.data() + i, kMarker, sizeof kMarker) == 0) {
                    float p[3];
                    std::memcpy(p, payload.data() + i - 28, sizeof p);
                    if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
                        e.spawn = {p[0], p[1], p[2]};
                        e.has_spawn = true;
                    }
                    break;
                }
            }
            std::lock_guard lock(g_actors_mutex);
            if (g_enemies.size() > 8192) g_enemies.clear();
            g_enemies[npc] = e;
        }
    }
    const uint32_t key = u32_at(payload, 3);
    // The sniffer also sees the packets we inject; never learn from those.
    if (key == kPuppetUser || key == kPuppetBody || u32_at(payload, 0x0E) == kPuppetPlayer ||
        u32_at(payload, 0x0E) == kPuppetHorse) {
        return;
    }

    std::lock_guard lock(g_mutex);
    if (keys::is_class(key, keys::kClassUser) && payload[0x0D] == off::kLoadKindMount) {
        g_horse_key = u32_at(payload, 0x0E);
        g_horse = std::move(payload);
        log::info("Capture: horse template (horse 0x{:08X}, {} bytes)", g_horse_key, g_horse.size());
        return;
    }
    if (keys::is_class(key, keys::kClassUser)) {
        const uint32_t actor = u32_at(payload, 0x0E);
        if (!keys::is_class(actor, keys::kClassPlayer)) return;  // horse, mercenaries: also user-owned
        // The first load of a session is a default character parked at
        // (0, 1000, 0) (apariencia-estado-coop.md §3): not the player's look.
        if (payload.size() > off::kLoadPositionFromEnd) {
            float p[3];
            std::memcpy(p, payload.data() + payload.size() - off::kLoadPositionFromEnd, sizeof p);
            if (std::fabs(p[0]) < 1.0f && std::fabs(p[1] - off::kLoadingParkHeight) < 1.0f &&
                std::fabs(p[2]) < 1.0f) {
                log::info("Capture: ignoring the default character load at (0, 1000, 0)");
                return;
            }
        }
        g_user = std::move(payload);
        g_user_key = key;
        g_player_key = actor;
        g_equip_key = actor;
        g_body.clear();  // a body seen before this user packet belongs to an older load
        log::info("Capture: user template (user 0x{:08X}, player 0x{:08X}, {} bytes)", key, actor,
                  g_user.size());
    } else if (keys::is_class(key, keys::kClassWorld) && g_player_key && g_body.empty() &&
               contains_u32(payload, g_player_key)) {
        // Many NPC loads also reference the player key (targets, relations), so
        // only the FIRST world actor after the user packet is the player's body
        // (verified on captura-2 by replaying this exact rule).
        g_body = std::move(payload);
        g_body_key = key;
        g_generation.fetch_add(1);
        log::info("Capture: body template (body 0x{:08X}, {} bytes) [generation {}]", key,
                  g_body.size(), g_generation.load());
    }
}

void capture_observe_c2s(const uint8_t* packet, size_t size) {
    aggro_observe_c2s(packet, size);
    enemy_observe_c2s(packet, size);
    suppress_observe_c2s(packet, size);
    hits_observe_c2s(packet, size);
    guest_hits_observe_c2s(packet, size);
    loot_observe_c2s(packet, size);
    piece4_observe_c2s(packet, size);
    attack_probe_observe_c2s(packet, size);
    if (size < 5 + 4) return;
    uint16_t op = 0;
    std::memcpy(&op, packet, 2);
    proto::EventKind kind;
    size_t expected = 0, ts_off = 0;
    if (op == off::kOpMoveActorReq) {
        kind = proto::EventKind::Move, expected = kMoveReqSize, ts_off = kMoveTs;
    } else if (op == off::kOpStartActionReq) {
        kind = proto::EventKind::StartAction, expected = kStartActionReqSize, ts_off = kStartActionTs;
    } else if (op == off::kOpRotateActorReq) {
        kind = proto::EventKind::Rotate, expected = kRotateReqSize, ts_off = kRotateTs;
    } else {
        return;
    }
    if (size != 5 + expected) return;

    Bytes payload(packet + 5, packet + size);
    {
        std::lock_guard lock(g_mutex);
        g_own_key = g_player_key;
        g_own_horse = g_horse_key;
    }
    const uint32_t key = u32_at(payload, 0);
    if (kind == proto::EventKind::Move && keys::is_class(key, keys::kClassWorld) && key != kPuppetBody &&
        key != kPuppetHorse && key != g_own_horse) {
        float p[3];
        std::memcpy(p, payload.data() + 0x04, sizeof p);
        std::lock_guard lock(g_actors_mutex);
        if (g_actors.size() > 4096) g_actors.clear();
        g_actors[key] = {{p[0], p[1], p[2]}, steady_ms()};
    }
    // The player's horse: its own (rare) messages are forwarded too.
    if (g_own_horse && key == g_own_horse) {
        if (kind == proto::EventKind::Move) {
            g_horse_last_move = payload;
            std::memcpy(g_horse_sent_pos, payload.data() + 0x04, sizeof g_horse_sent_pos);
        }
        emit(kind, std::move(payload));
        return;
    }
    // Otherwise only the local player's own messages (NPC AI uses the same opcodes).
    if (!g_own_key || key != g_own_key) return;

    uint64_t ts = 0;
    std::memcpy(&ts, payload.data() + ts_off, 8);
    note_clock(ts);
    if (kind == proto::EventKind::Move && g_own_horse) {
        // Riding: Path.parent (+0x8C) is the horse and the receiver derives
        // the horse's moves from ours.
        uint32_t parent = 0;
        std::memcpy(&parent, payload.data() + 0x8C, 4);
        if (parent == g_own_horse) {
            g_ridden_until_ms = steady_ms() + 200;
            std::memcpy(g_horse_sent_pos, payload.data() + 0x94, sizeof g_horse_sent_pos);
        }
    }
    if (kind == proto::EventKind::Move) {
        g_last_move = payload;
        g_last_move_ms = steady_ms();
        g_last_move_ts = ts;
        std::memcpy(g_last_sent_pos, payload.data() + 0x04, sizeof g_last_sent_pos);
    }
    emit(kind, std::move(payload));
}

void capture_tick() {
    {
        std::lock_guard lock(g_mutex);
        if (!g_sink) return;
        g_own_key = g_player_key;
    }
    if (!g_own_key) return;
    if (!g_reader_ready) g_reader_ready = g_reader.init();
    if (!g_reader_ready) return;
    const uint64_t now = steady_ms();
    drain_equip();
    capture_wallup(now);
    capture_synth(now);
    capture_horse_synth(now);
}

bool capture_install_equip_hook() {
    if (g_equip_hook) return true;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t fn = base + off::kServerOtherEquipWriter;
    uint8_t head[sizeof off::kServerOtherEquipWriterHead];
    if (base != off::kImageBase || !game::safe_read(fn, head, sizeof head) ||
        std::memcmp(head, off::kServerOtherEquipWriterHead, sizeof head) != 0) {
        log::warn("Capture: OtherEquipItemAck writer not recognised; equipment changes won't be sent");
        return false;
    }
    g_equip_hook = safetyhook::create_mid(reinterpret_cast<void*>(fn), on_equip_writer);
    log::info("Capture: equipment hook {}", g_equip_hook ? "installed" : "FAILED");
    // The weapon docking relay is not needed: the copy draws and sheathes its
    // weapon on its own from the replayed actions (seen in game since the first
    // mirror). The hook below also never fired in single player.
    if (constexpr bool kDockingRelay = false; !kDockingRelay) return static_cast<bool>(g_equip_hook);
    const uintptr_t dock = base + off::kServerDockingWriter;
    uint8_t dh[sizeof off::kServerDockingWriterHead];
    if (game::safe_read(dock, dh, sizeof dh) &&
        std::memcmp(dh, off::kServerDockingWriterHead, sizeof dh) == 0) {
        g_dock_hook = safetyhook::create_mid(reinterpret_cast<void*>(dock), on_dock_writer);
        log::info("Capture: weapon docking hook {}", g_dock_hook ? "installed" : "FAILED");
    } else {
        log::warn("Capture: docking writer not recognised; weapon draw won't be sent");
    }
    return static_cast<bool>(g_equip_hook);
}

void capture_set_sink(EventSink sink) {
    std::lock_guard lock(g_mutex);
    g_sink = std::move(sink);
}

std::optional<proto::SpawnInfo> capture_spawn_info(const proto::Vec3& pos, const proto::Quat& rot) {
    std::lock_guard lock(g_mutex);
    if (g_user.empty() || g_body.empty()) return std::nullopt;
    proto::SpawnInfo info;
    info.generation = ++g_spawn_counter;
    info.user_key = g_user_key;
    info.player_key = g_player_key;
    info.body_key = g_body_key;
    info.pos = pos;
    info.rot = rot;
    info.user = g_user;
    info.body = g_body;
    if (!g_horse.empty()) {
        info.horse_key = g_horse_key;
        info.horse = g_horse;
        // Where the horse is now (the template has its load position).
        if (auto hp = game::actor_world_position(g_horse_key)) info.horse_pos = *hp;
        else info.horse_key = 0, info.horse.clear();  // not around: leave it out
    }
    // Gear changed since the templates were captured (the writer thread
    // updates it, so copy under its lock).
    std::lock_guard slots_lock(g_slots_mutex);
    if (g_equip_generation == g_generation.load() && !g_equip_state.empty()) {
        info.equip = g_equip_state.snapshot(g_player_key);
    }
    return info;
}

uint32_t capture_template_generation() { return g_generation.load(); }

uint32_t capture_horse_present() {
    std::lock_guard lock(g_mutex);
    if (g_horse.empty() || !g_horse_key) return 0;
    return game::actor_world_position(g_horse_key) ? g_horse_key : 0;
}

EnemyState capture_enemy_state(uint32_t key) {
    std::lock_guard lock(g_actors_mutex);
    auto it = g_enemies.find(key);
    if (it == g_enemies.end()) return {};
    return {it->second.hp, it->second.dead};
}

bool capture_key_alarm() { return g_key_alarm.load(); }

bool capture_is_enemy(uint32_t key) {
    std::lock_guard lock(g_actors_mutex);
    return g_enemies.count(key) != 0;
}

uint32_t capture_own_user_key() {
    std::lock_guard lock(g_mutex);
    return g_user_key;
}

uint32_t capture_own_player_key() {
    std::lock_guard lock(g_mutex);
    return g_player_key;
}

std::optional<NearbyActor> capture_nearest_actor(const proto::Vec3& from, float max_m) {
    std::optional<NearbyActor> best;
    const uint64_t now = steady_ms();
    std::lock_guard lock(g_actors_mutex);
    for (const auto& [key, a] : g_actors) {
        if (now - a.ms > 3000 || !g_has_hp.count(key)) continue;
        const float dx = a.pos.x - from.x, dy = a.pos.y - from.y, dz = a.pos.z - from.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= max_m && (!best || d < best->distance)) best = NearbyActor{key, a.pos, d};
    }
    return best;
}

uint32_t capture_last_attack_hash() { return g_last_attack_hash.load(); }
uint32_t capture_last_attack_target() { return g_last_attack_target.load(); }

void capture_set_damage_enabled(bool on) {
    g_damage_enabled = on;
    log::info("Co-op: mirrored damage {}", on ? "on" : "off");
}

// Mirrored damage asks us to hurt one of OUR world's NPCs through our own
// server on the other player's word, so it is bounded (security review H1,
// seguridad-combate.md): only a hostile enemy that is fighting
// (enemy_hostile_in_combat) whose HP we know (a VaryHPAck of its HP stat:
// it has one), never below 1 HP, at most this much per event and this often.
namespace {
constexpr int64_t kMaxRemoteDamage = 200000;  // 200 HP x1000; the strongest measured blow is 56 HP
constexpr double kDamageBurst = 10, kDamagePerS = 10;
double g_damage_tokens = kDamageBurst;  // network thread
uint64_t g_damage_tokens_ms = 0;
uint64_t g_damage_refused = 0;
}  // namespace

bool apply_remote_damage(const std::vector<uint8_t>& ev) {
    if (!g_damage_enabled || ev.size() != proto::kDamageEventSize) return false;
    uint32_t type = 0;
    float p[3];
    int64_t delta = 0;
    std::memcpy(&type, ev.data(), 4);
    std::memcpy(p, ev.data() + 4, sizeof p);
    std::memcpy(&delta, ev.data() + 16, 8);
    const bool killed = ev[24] != 0;
    if (!type || delta >= 0 || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    {
        // The rate first: each event below walks every NPC we know.
        const uint64_t t = steady_ms();
        g_damage_tokens = std::min(kDamageBurst, g_damage_tokens + static_cast<double>(t - g_damage_tokens_ms) *
                                                                       kDamagePerS / 1000.0);
        g_damage_tokens_ms = t;
        if (g_damage_tokens < 1.0) {
            if (g_damage_refused++ % 20 == 0) {
                log::info("Co-op: damage for enemy type 0x{:X} refused: too many [{} refused]", type,
                          g_damage_refused);
            }
            return false;
        }
        g_damage_tokens -= 1.0;
    }

    // Our enemy of the same variant, alive, with known hit points, whose SPAWN
    // point is nearest to the sender's enemy's spawn point (both worlds spawn
    // the same enemies at the same places; where they are now drifts apart as
    // each chases its own player). Without a known spawn point, where it is now.
    constexpr float kMaxDistance = 25.0f;
    struct Match {
        uint32_t key;
        float d;
        int64_t hp;
    };
    std::vector<Match> matches;
    const uint64_t now = steady_ms();
    {
        std::lock_guard lock(g_actors_mutex);
        for (const auto& [key, e] : g_enemies) {
            // Only character loads (record type 3) are in g_enemies; objects
            // ("Game Mechanic", record type 7) are not, so no stat-less target.
            if (e.type != type || e.dead || e.hp <= 0 || e.hp_max <= 0) continue;
            auto a = g_actors.find(key);
            if (a == g_actors.end() || now - a->second.ms > 10000) continue;  // not around us
            const proto::Vec3 at = e.has_spawn ? e.spawn : a->second.pos;
            const float dx = at.x - p[0], dy = at.y - p[1], dz = at.z - p[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < kMaxDistance) matches.push_back({key, d, e.hp});
        }
    }
    uint32_t best = 0;
    float best_d = kMaxDistance;
    int64_t best_hp = 0;
    for (const Match& m : matches) {
        // Outside g_actors_mutex: enemy_relay takes its own lock.
        if (m.d < best_d && enemy_hostile_in_combat(m.key)) best = m.key, best_d = m.d, best_hp = m.hp;
    }
    if (!best && !matches.empty()) {
        if (g_damage_refused++ % 20 == 0) {
            log::info("Co-op: damage for enemy type 0x{:X} refused: none of our {} matching enemies is fighting us "
                      "[{} refused]",
                      type, matches.size(), g_damage_refused);
        }
        return false;
    }
    if (!best) {
        if (g_damage_unmatched++ % 10 == 0) {
            // Why: how many enemies of that variant we know, alive and around.
            int known = 0, alive = 0, around = 0;
            float nearest = -1;
            {
                std::lock_guard lock(g_actors_mutex);
                for (const auto& [key, e] : g_enemies) {
                    if (e.type != type) continue;
                    ++known;
                    if (e.dead || e.hp == 0) continue;
                    ++alive;
                    auto a = g_actors.find(key);
                    if (a == g_actors.end() || now - a->second.ms > 10000) continue;
                    ++around;
                    const proto::Vec3 at = e.has_spawn ? e.spawn : a->second.pos;
                    const float d = std::sqrt((at.x - p[0]) * (at.x - p[0]) + (at.y - p[1]) * (at.y - p[1]) +
                                              (at.z - p[2]) * (at.z - p[2]));
                    if (nearest < 0 || d < nearest) nearest = d;
                }
            }
            log::info("Co-op: damage for enemy type 0x{:X} at ({:.0f}, {:.0f}, {:.0f}) found no match: "
                      "{} known, {} alive, {} around us, nearest spawn {:.1f} m [{} unmatched]",
                      type, p[0], p[1], p[2], known, alive, around, nearest, g_damage_unmatched);
        }
        return false;
    }
    // VaryStatByAccInputReq takes the generic stat path: at 0 HP the enemy is
    // only "knocked out", never dead (pieza4-dano-servidor.md §2.d: 53 enemies
    // left at 0 HP in the tests, 3 kept fighting). Never below 1 HP; the kill
    // must come from a native blow.
    int64_t amount = std::max(delta, -kMaxRemoteDamage);
    amount = std::max(amount, -(best_hp - 1000));
    if (amount >= 0) return false;

    // VaryStatByAccInputReq: u32 key | u32 stat | i64 delta (x1000).
    Bytes pkt(5 + 16);
    const uint16_t op = kOpVaryStatByAccInputReq, len = 16;
    std::memcpy(pkt.data(), &op, 2);
    pkt[2] = 0;
    std::memcpy(pkt.data() + 3, &len, 2);
    std::memcpy(pkt.data() + 5, &best, 4);
    std::memcpy(pkt.data() + 9, &kStatHP, 4);
    std::memcpy(pkt.data() + 13, &amount, 8);
    const bool ok = sniffer_inject_c2s(std::move(pkt));
    if (ok && g_damage_applied++ < 100) {
        log::info("Co-op: other player's hit applied to our enemy 0x{:08X} ({:.1f} m away, {:.1f} hp{})", best,
                  best_d, amount / 1000.0, killed ? ", killed" : "");
    }
    return ok;
}

bool capture_game_clock_trusted() { return g_clock_reader.load(std::memory_order_relaxed) == 1; }
bool capture_game_clock_decided() { return g_clock_reader.load(std::memory_order_relaxed) != 0; }

uint64_t capture_game_clock_ms() {
    if (g_clock_reader.load(std::memory_order_relaxed) == 1) {
        if (const auto c = game::client_game_clock_ms()) return *c;
    }
    const uint64_t ts = g_clock_ts.load(std::memory_order_relaxed);
    if (ts == 0) return 0;
    return ts + (steady_ms() - g_clock_at.load(std::memory_order_relaxed));
}

uint64_t event_timestamp(const proto::GameEvent& ev) {
    size_t at = 0, size = 0;
    switch (ev.kind) {
        case proto::EventKind::Move: at = kMoveTs, size = ev.payload.size() == kMoveWireSize ? kMoveWireSize : kMoveReqSize; break;
        case proto::EventKind::StartAction: at = kStartActionTs, size = kStartActionReqSize; break;
        case proto::EventKind::Rotate: at = kRotateTs, size = kRotateReqSize; break;
        case proto::EventKind::WallUp:
        case proto::EventKind::Equip:
        case proto::EventKind::Docking:
        case proto::EventKind::Relay:
        case proto::EventKind::Damage:
        case proto::EventKind::EnemyAct:
        case proto::EventKind::EnemyPos:
        case proto::EventKind::PlayerHit:
        case proto::EventKind::GuestHit: return 0;
    }
    if (ev.payload.size() != size) return 0;
    uint64_t ts = 0;
    std::memcpy(&ts, ev.payload.data() + at, 8);
    return ts;
}

}  // namespace cdmp::net
