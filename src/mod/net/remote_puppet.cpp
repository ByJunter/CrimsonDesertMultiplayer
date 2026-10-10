#include "mod/net/remote_puppet.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <map>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>

#include <safetyhook.hpp>

#include "common/equip_state.h"
#include "common/inject_clock.h"
#include "mod/game/actor_lookup.h"
#include "mod/game/game_clock.h"
#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/enemy_relay.h"
#include "mod/net/horse_diag.h"
#include "common/keys.h"
#include "mod/net/local_capture.h"

namespace cdmp::net {
namespace {
namespace off = game::build_2976;
using Bytes = std::vector<uint8_t>;

// Fresh keys for the puppet. Real serials seen so far are below 0x1000, so
// 0xF001 will not collide with anything the server allocates in a session.
constexpr uint32_t kPuppetUser = 0x9010F001;
constexpr uint32_t kPuppetPlayer = 0xA010F001;
constexpr uint32_t kPuppetBody = 0xB010F001;
// The sender's horse (caballo-coop.md §2.3) and the extra member of the
// sender's party list inside the horse packet.
constexpr uint32_t kPuppetHorse = 0xB010F002;
constexpr uint32_t kPuppetHorseParty = 0xB010F003;
// Slots of a user's child container that hold its mounts (2 = horse, 3 = other mounts).
constexpr uint8_t kFirstMountSlot = 2, kLastMountSlot = 7;

// The character number in the player's load packets (user +0x17/+0x43, body
// +0x1839/+0x1865). The client keeps ONE equipment-list cache keyed by it and
// applies cached lists to the first actor with that number: a copy sharing the
// local player's number mixed up both players' equipment (live test 1,
// apariencia-estado-coop.md §5). Give the copy its own.
constexpr uint64_t kSenderCharacterNo = 0x7FFFFFFFFFFFFFFF;
constexpr uint64_t kPuppetCharacterNo = 0x7FFFFFFFFFFFF001;

constexpr size_t kCharacterNoAt[] = {0x17, 0x43};

// ---------------------------------------------------------------- checks on the other player's data
// Its load templates go into our game as if our own server had sent them, so
// they must look like a player load and name only the copy's keys (security
// review H1, docs/research/seguridad/). Record: u16 len | u8 type | u32 key
// | u32 owner | u8 index in the owner's child container | ...
uint32_t rd32(const Bytes& v, size_t off) {
    uint32_t x = 0;
    std::memcpy(&x, v.data() + off, sizeof x);
    return x;
}

struct RecordHead {
    uint8_t type;
    uint32_t key, owner;
    uint8_t index;
};

std::optional<std::vector<RecordHead>> record_chain(const Bytes& v) {
    std::vector<RecordHead> out;
    size_t at = 0;
    while (at + 2 <= v.size()) {
        uint16_t len;
        std::memcpy(&len, v.data() + at, 2);
        at += 2;
        if (len < 5 || at + len > v.size() || out.size() >= 8) return std::nullopt;
        RecordHead h{v[at], rd32(v, at + 1), 0, 0xFF};
        if (len >= 10) {
            h.owner = rd32(v, at + 5);
            h.index = v[at + 9];
        }
        out.push_back(h);
        at += len;
    }
    if (at != v.size()) return std::nullopt;
    return out;
}

bool head_is(const RecordHead& h, uint8_t type, uint32_t key, uint32_t owner, uint8_t index) {
    return h.type == type && h.key == key && h.owner == owner && h.index == index;
}

// None of OUR own user's or player's keys may be left in a template.
bool names_our_player(const Bytes& v) {
    const uint32_t ours[] = {capture_own_user_key(), capture_own_player_key()};
    for (size_t i = 0; i + 4 <= v.size(); ++i) {
        const uint32_t k = rd32(v, i);
        for (uint32_t o : ours) {
            if (o && k == o) return true;
        }
    }
    return false;
}

bool set_character_no(Bytes& user, Bytes& body) {
    if (user.size() < 0x60 || body.size() < 0x60) return false;
    auto patch = [](Bytes& v, size_t base) {
        for (size_t at : kCharacterNoAt) {
            if (base + at + 8 > v.size()) return false;
            uint64_t x;
            std::memcpy(&x, v.data() + base + at, 8);
            if (x != kSenderCharacterNo) return false;
        }
        for (size_t at : kCharacterNoAt) std::memcpy(v.data() + base + at, &kPuppetCharacterNo, 8);
        return true;
    };
    // The body's copy of the user record starts with the same 0x17 header bytes.
    size_t base = std::string::npos;
    for (size_t i = 1; i + 0x17 <= body.size(); ++i) {
        if (std::memcmp(body.data() + i, user.data(), 0x17) == 0) {
            base = i;
            break;
        }
    }
    if (base == std::string::npos) return false;
    Bytes u = user, b = body;
    if (!patch(u, 0) || !patch(b, base)) return false;
    user = std::move(u);
    body = std::move(b);
    return true;
}

bool is_actor_key(uint32_t v) { return keys::is_actor_key(v); }
bool is_puppet_key(uint32_t v) {
    return v == kPuppetUser || v == kPuppetPlayer || v == kPuppetBody || v == kPuppetHorse ||
           v == kPuppetHorseParty;
}
// True if a key of some other actor (of the sender's world) is left anywhere.
bool has_foreign_key(const Bytes& v) {
    for (size_t i = 0; i + 4 <= v.size(); ++i) {
        uint32_t x;
        std::memcpy(&x, v.data() + i, 4);
        if (is_actor_key(x) && !is_puppet_key(x)) return true;
    }
    return false;
}

// Sideways offset (world X, metres) of the puppet relative to the sender.
// 0 = the puppet walks exactly the sender's path; then climbing etc. lines up
// with the real geometry.
std::atomic<float> g_offset_x{0.0f};

uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// Replaces every (unaligned) occurrence; packets are packed, keys can sit anywhere.
int replace_u32(Bytes& v, uint32_t from, uint32_t to) {
    int n = 0;
    for (size_t i = 0; i + 4 <= v.size(); ++i) {
        if (std::memcmp(v.data() + i, &from, 4) == 0) {
            std::memcpy(v.data() + i, &to, 4);
            ++n;
            i += 3;
        }
    }
    return n;
}

uint32_t u32_at(const Bytes& v, size_t off) {
    uint32_t x = 0;
    std::memcpy(&x, v.data() + off, sizeof x);
    return x;
}

Bytes make_packet(uint16_t op, const Bytes& payload) {
    Bytes p(5 + payload.size());
    const uint16_t len = static_cast<uint16_t>(payload.size());
    std::memcpy(p.data(), &op, 2);
    p[2] = 0xFF;  // slot -1, as in every captured load/remove packet
    std::memcpy(p.data() + 3, &len, 2);
    std::memcpy(p.data() + 5, payload.data(), payload.size());
    return p;
}

// ---------------------------------------------------------------- spawn state
std::mutex g_mutex;  // spawn state (never taken on game threads)
bool g_spawned = false;
std::atomic<uint32_t> g_src_player_key{0};  // the sender's own player key
std::atomic<uint32_t> g_src_horse_key{0};   // the sender's horse key (0 = no horse)
std::atomic<uint32_t> g_src_user_key{0};    // the sender's user key
std::atomic<uint32_t> g_sender_horse{0};    // the sender's horse key, even when not loaded here
bool g_horse_spawned = false;   // a horse copy belongs to this puppet (pending or loaded)
// The horse's load waits for the sender horse's first StartAction: a copy
// with no action running crashed the game (caballo-coop.md §9.2). Both are
// then queued together, the action right behind the load.
Bytes g_horse_pending;           // its load packet, until then (under g_mutex)
// Dismount: the copy has no real rider<->horse link, so RideOff::Enter fails
// and the rider freezes mid-animation (live test 2026-10-03). Only the rider
// is reloaded; the horse copy stays in the world (under g_mutex).
bool g_keep_horse = false;       // the next removal keeps the horse
bool g_horse_kept = false;       // a horse copy is in the world without its rider
uint64_t g_horse_not_before = 0; // and not before this (player packets first)
std::atomic<bool> g_horse_enabled{false};
std::atomic<bool> g_equip_enabled{true};
// Serial of the equipment messages we inject. The client applies an
// OtherEquipItemAck only if its serial is above the copy's last one
// (comp+0x18), and a reloaded copy may carry the serial of an earlier
// injection through the equipment cache, so ours only grow, and never stay
// below the sender's (ropa-al-desmontar.md section 5.3).
std::mutex g_equip_mutex;
uint32_t g_equip_serial_out = 0;               // under g_equip_mutex
std::atomic<uint64_t> g_equip_check_at{0};    // when to log the copy's gear (diagnostic)

uint32_t next_equip_serial(uint32_t sender_serial) {
    std::lock_guard lock(g_equip_mutex);
    g_equip_serial_out = std::max(g_equip_serial_out + 1, sender_serial);
    return g_equip_serial_out;
}

// OtherUnequipItemAck: u32 key | u16 slot | u32 serial (no fragment byte).
Bytes unequip_payload(uint32_t key, uint16_t slot, uint32_t serial) {
    Bytes un(10);
    std::memcpy(un.data(), &key, 4);
    std::memcpy(un.data() + 4, &slot, 2);
    std::memcpy(un.data() + 6, &serial, 4);
    return un;
}

// ---------------------------------------------------------------- injection queue
struct Pending {
    uint64_t due_ms;  // not delivered before this time
    bool movement;    // purged when the puppet goes away
    Bytes packet;
};

SRWLOCK g_queue_lock = SRWLOCK_INIT;  // the NetworkThread only ever try-acquires
std::deque<Pending> g_queue;          // ordered by due time
constexpr uint64_t kStaleMovementMs = 2000;
uint64_t g_stale_dropped = 0;  // under g_queue_lock
// Movement timing at delivery (analisis-ia2/P3 §7), per class: how late a
// sample went in after it was due (queue wait), and its margin to the
// client's render clock (stamp - (game clock - 200 ms); < 0: late, drawn
// past). Under g_queue_lock; written out every 10 s by puppet_tick.
struct Timing {
    std::vector<int64_t> wait, margin;
};
Timing g_timing[2];  // 0 the other player (and horse), 1 enemy copies
// The last position we told the copy to be at (a delivered MoveActorAck) and
// when, under g_queue_lock: puppet_tick reloads a copy that stopped following
// them (live test, 4 Oct 22:08: after dying twice through D1/D2 and five
// minutes dead, the copy played its actions in place and ignored its moves).
float g_told_pos[3] = {};
std::atomic<float> g_told_x{NAN}, g_told_z{NAN};  // the same, for the tree bend on actor-update threads
uint64_t g_told_ms = 0;
// Movement timestamps checked against the client's clock at delivery, and
// whether this game is paused in a menu (then no movement goes in: it would
// sit in the copy's buffer and spoil it; pausa-copia-congelada.md F1).
// Under g_queue_lock.
InjectClock g_inject_clock;
PauseDetector g_local_pause;
std::atomic<bool> g_paused_now{false};
bool g_paused_logged = false;  // puppet_tick only
std::atomic<uint64_t> g_restamped{0};
std::atomic<uint64_t> g_dropped_repeats{0};
uint64_t g_restamped_logged = 0;  // puppet_tick only

// Inserts keeping the queue sorted by due time (gap-fill samples are created
// after later packets were already queued). Stable for equal due times.
// A flood from the other player must not grow the queue without limit (each
// entry goes into the game): beyond this, new packets are dropped.
constexpr size_t kMaxQueued = 8192;
std::atomic<uint64_t> g_queue_full_drops{0};

void enqueue_at(Bytes packet, uint64_t due_ms, bool movement) {
    if (packet.size() > 5 + 0x7FC1) return;  // more than the game's own packets can hold
    AcquireSRWLockExclusive(&g_queue_lock);
    if (g_queue.size() >= kMaxQueued) {
        // Full: the oldest movement makes room for anything else; movement waits its turn.
        auto old = movement ? g_queue.end()
                            : std::find_if(g_queue.begin(), g_queue.end(), [](const Pending& p) { return p.movement; });
        if (old == g_queue.end()) {
            ReleaseSRWLockExclusive(&g_queue_lock);
            g_queue_full_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_queue.erase(old);
        g_queue_full_drops.fetch_add(1, std::memory_order_relaxed);
    }
    auto it = std::upper_bound(g_queue.begin(), g_queue.end(), due_ms,
                               [](uint64_t due, const Pending& p) { return due < p.due_ms; });
    g_queue.insert(it, {due_ms, movement, std::move(packet)});
    ReleaseSRWLockExclusive(&g_queue_lock);
}

// The other player's copy (and horse) movement only: enemy copies' is theirs.
void purge_movement() {
    AcquireSRWLockExclusive(&g_queue_lock);
    std::erase_if(g_queue, [](const Pending& p) {
        if (!p.movement) return false;
        uint32_t key = 0;
        if (p.packet.size() >= 9) std::memcpy(&key, p.packet.data() + 5, 4);
        return (key & 0xFFFF0000u) != 0xB01F0000u;
    });
    ReleaseSRWLockExclusive(&g_queue_lock);
}

// ---------------------------------------------------------------- Req -> Ack recipes
// From docs/research/mirror-ack-formats.md (disassembly of both handlers; both
// sides share the struct deserializers, only the outer wrapper differs). The
// Ack handler drops a packet silently unless it consumes exactly payload_len
// bytes, so sizes must be exact.

Bytes slice(const Bytes& v, size_t from, size_t to) {
    return Bytes(v.begin() + static_cast<ptrdiff_t>(from), v.begin() + static_cast<ptrdiff_t>(to));
}
void append(Bytes& out, const Bytes& part) { out.insert(out.end(), part.begin(), part.end()); }

// Parent/mount key: when the sender rides, it points at the sender's horse,
// which the puppet must not try to mount. All-FF = no parent.
// Riding the sender's horse is kept: its key was translated to our copy of
// the horse, which exists here.
void clear_parent(Bytes& ack, size_t off) {
    if (off + 8 > ack.size()) return;
    uint32_t parent = 0;
    std::memcpy(&parent, ack.data() + off, 4);
    if (parent == kPuppetHorse && g_horse_enabled) return;
    std::memset(ack.data() + off, 0xFF, 8);
}

// A field that may name another actor (mirror-ack-formats.md): an actor of the
// sender's world, unknown or different here, never one of ours (security
// review H6).
void scrub_key(Bytes& ack, size_t off) {
    if (off + 4 > ack.size()) return;
    uint32_t v;
    std::memcpy(&v, ack.data() + off, 4);
    if (is_actor_key(v) && !is_puppet_key(v)) std::memset(ack.data() + off, 0, 4);
}

// Floats that reach the game's physics and animation: finite, inside the
// world, and a rotation that is one (security review H3).
bool sane_pos(const Bytes& v, size_t off) {
    if (off + 12 > v.size()) return false;
    float p[3];
    std::memcpy(p, v.data() + off, sizeof p);
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
           std::fabs(p[0]) <= proto::kMaxWorldCoord && std::fabs(p[2]) <= proto::kMaxWorldCoord &&
           std::fabs(p[1]) <= proto::kMaxWorldHeight;
}
bool sane_quat(const Bytes& v, size_t off) {
    if (off + 16 > v.size()) return false;
    float q[4];
    std::memcpy(q, v.data() + off, sizeof q);
    const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return std::isfinite(n) && n > 0.5f && n < 1.5f;
}
bool finite_at(const Bytes& v, size_t off, int count) {
    if (off + 4 * static_cast<size_t>(count) > v.size()) return false;
    for (int i = 0; i < count; ++i) {
        float f;
        std::memcpy(&f, v.data() + off + 4 * i, 4);
        if (!std::isfinite(f) || std::fabs(f) > 1e7f) return false;
    }
    return true;
}

void shift_x(Bytes& ack, size_t off) {
    if (off + 4 > ack.size()) return;
    float x;
    std::memcpy(&x, ack.data() + off, 4);
    if (std::isfinite(x)) {
        x += g_offset_x;
        std::memcpy(ack.data() + off, &x, 4);
    }
}

// Sender's latest (unshifted) world position, from its Moves. Used to
// recognise which optional vec3 fields in other messages are positions near
// the sender (and therefore must be shifted too).
float g_ref_pos[3] = {NAN, NAN, NAN};

// Shifts X of the vec3 at `off` only if it lies within 30 m of the sender:
// those fields are not always positions (RotateActor 0x14/0x5C held one in
// only 6-15% of captured packets).
void shift_x_if_near_sender(Bytes& ack, size_t off) {
    if (off + 12 > ack.size() || !std::isfinite(g_ref_pos[0])) return;
    float v[3];
    std::memcpy(v, ack.data() + off, sizeof v);
    const float dx = v[0] - g_ref_pos[0], dy = v[1] - g_ref_pos[1], dz = v[2] - g_ref_pos[2];
    if (dx * dx + dy * dy + dz * dz < 30.0f * 30.0f) shift_x(ack, off);
}

// The sender's clock (ms) drives the receiver's interpolation. Events reach
// us late (network, playout delay) and on another game's clock, so move every
// timestamp onto our clock plus the delay; otherwise the client sees stale
// samples and snaps ("lag teleports").
int64_t g_ts_shift = 0;  // for the event being processed (event thread only)

void rebase_timestamp(Bytes& ack, size_t off) {
    if (off + 8 > ack.size()) return;
    uint64_t ts;
    std::memcpy(&ts, ack.data() + off, 8);
    // Saturated: an action recorded before this game started would wrap to
    // ~1.8e19 ms, "never" for the client (a copy left at the origin, 7 Oct).
    const int64_t shifted = static_cast<int64_t>(ts) + g_ts_shift;
    ts = shifted < 1 ? 1 : static_cast<uint64_t>(shifted);
    std::memcpy(ack.data() + off, &ts, 8);
}

// MoveActor: 514 -> 235. Ack = req[0:0xDE] + u8 0 ("also send to controller",
// read and ignored by the client) + req[0xDE:0xEA]; the Req's trailing
// 10 x 28-byte history array is not part of the Ack.
std::optional<Bytes> move_ack(const Bytes& req) {
    if (req.size() != 514) return std::nullopt;
    if (!sane_pos(req, 0x04) || !sane_quat(req, 0x10) || !sane_pos(req, 0x4C) || !finite_at(req, 0x58, 4) ||
        !finite_at(req, 0x94, 7)) {
        return std::nullopt;
    }
    std::memcpy(g_ref_pos, req.data() + 0x04, sizeof g_ref_pos);
    Bytes ack = slice(req, 0, 0xDE);
    ack.push_back(0);
    append(ack, slice(req, 0xDE, 0xEA));
    shift_x(ack, 0x04);
    shift_x(ack, 0x4C);
    clear_parent(ack, 0x8C);
    scrub_key(ack, 0x20);
    rebase_timestamp(ack, 0xD5);  // advances 25 ms per packet in captura-2
    return ack;
}

// StartAction: 725 -> 725, same bytes but the Req's last u8 (a "force" flag)
// moves to 0x8E, before the big block.
std::optional<Bytes> start_action_ack(const Bytes& req) {
    if (req.size() != 725 || !finite_at(req, 0x8E, 7) || !finite_at(req, 0xD6, 3)) return std::nullopt;
    Bytes ack = slice(req, 0, 0x8E);
    ack.push_back(req[0x2D4]);
    append(ack, slice(req, 0x8E, 0x2D4));
    clear_parent(ack, 0x117);
    scrub_key(ack, 0xAB);
    scrub_key(ack, 0x169);
    // Action target (Req +0x16C, Ack +0x16D): 0 almost always; when it names
    // another actor (an enemy, a grabbed animal) it is one of the SENDER's
    // world, which here may be a different actor (interacciones-mundo.md R3).
    if (ack.size() >= 0x171) {
        uint32_t target;
        std::memcpy(&target, ack.data() + 0x16D, 4);
        if (is_actor_key(target) && !is_puppet_key(target)) std::memset(ack.data() + 0x16D, 0, 4);
    }
    // World positions found in captura-2 at Req 0x8E (393/400 packets) and
    // 0xD6 (247/400); they sit one byte later in the Ack. Unshifted, they
    // snapped the puppet onto the sender every ~0.5 s.
    shift_x_if_near_sender(ack, 0x8F);
    shift_x_if_near_sender(ack, 0xD7);
    // StartAction feeds the same position buffer as MoveActor; its u64
    // timestamp (Ack+0x160) needs the same rebase (movimiento-remoto-cliente.md).
    rebase_timestamp(ack, 0x160);
    return ack;
}

// RotateActor: 176 -> 177. Ack = req[0:0xA4] + u8 0 + req[0xA4:0xB0].
std::optional<Bytes> rotate_ack(const Bytes& req) {
    if (req.size() != 176 || !sane_quat(req, 0x04) || !finite_at(req, 0x14, 7)) return std::nullopt;
    Bytes ack = slice(req, 0, 0xA4);
    ack.push_back(0);
    append(ack, slice(req, 0xA4, 0xB0));
    clear_parent(ack, 0x54);
    rebase_timestamp(ack, 0x9C);
    shift_x_if_near_sender(ack, 0x14);
    shift_x_if_near_sender(ack, 0x5C);
    return ack;
}

// UpdateWallUpData: Ack = Req (132 bytes, no extra byte). Points (not
// directions) follow the puppet's sideways offset.
std::optional<Bytes> wallup_ack(const Bytes& req) {
    if (req.size() != 132) return std::nullopt;
    for (size_t at : {size_t{0x09}, size_t{0x15}, size_t{0x21}, size_t{0x2D}, size_t{0x39}, size_t{0x45},
                      size_t{0x5C}}) {
        if (!finite_at(req, at, 3)) return std::nullopt;
    }
    Bytes ack = req;
    for (size_t at : {size_t{0x09}, size_t{0x15}, size_t{0x21}, size_t{0x2D}, size_t{0x39},
                      size_t{0x45}, size_t{0x5C}}) {
        shift_x(ack, at);
    }
    return ack;
}

// ---------------------------------------------------------------- event-thread state
std::atomic<uint64_t> g_events{0};
// Riding state of the sender, from Path.parent (+0x8C) of its Moves.
bool g_sender_riding = false;
int g_unmounted_moves = 0;
std::atomic<bool> g_reload_request{false};
uint64_t g_relays_dropped = 0;

// Scene objects (trees) that are gimmicks in OUR world now, by scene id
// (UpdateOnDemandGimmickAck, sniffer thread). The other player's tree hook
// goes in only on a tree loaded here: the same tree has the same scene id on
// every PC (Shadow test, 6 Oct), but each PC loads it only near its player.
using SceneId = std::array<uint8_t, 16>;
std::mutex g_gimmick_mutex;
std::set<SceneId> g_scene_gimmicks;
// The piece (physics body) id of each tree in OUR world, from our own hooks:
// the hook's +0x1E piece id is made by the client that throws the rope and
// exists only there, so the sender's one fails here (0x5A5370 finds no such
// body; arbol-cuerda-fallo.md). Forgotten whenever the tree loads or unloads
// (ids change with every load).
std::map<SceneId, uint64_t> g_own_pieces;
constexpr size_t kRemoteCatchPiece = 0x1E;       // u64; -1 = locate by the scene id
constexpr size_t kRemoteCatchPoint = 0x26;       // vec3, local to the piece (or the scene object)
uint64_t g_hooks_shown = 0, g_hooks_not_here = 0, g_hooks_released = 0;
// The copy's rope is on a tree (network thread). The server never sends the
// catcher its own ReleaseRemoteCatchResultAck (only the others got it in the
// MMO), so the copy is released here when the sender lets go: without it the
// copy stays hooked (the blue mark stays on the tree) and every later hook
// fails ("RemoteCatchResultAck processing failed", live test 6 Oct).
std::atomic<bool> g_copy_hooked{false};
uint8_t g_hook_slot = 0;
uint64_t g_rope_seen_ms = 0;  // the sender's last rope action (network thread)
// The rope ends at the sender's first main-layer StartAction (+8, the new
// action, not 0) outside this set: letting go with the launch (1F4EF471),
// without it (D20295AD), being hit, falling... (12 of 12 hooks in the
// captures, arbol-cuerda-2.md). The copy's own release frame events never
// run, so the mod releases it.
constexpr uint32_t kRopeActions[] = {0x9A368460, 0x2821F20A, 0xA747BF2F, 0x7CC69D61,
                                     0xD9C333C4, 0x76D8C2D8, 0x6B4EE3CA, 0x563271FE};
bool is_rope_action(uint32_t a) { return std::find(std::begin(kRopeActions), std::end(kRopeActions), a) != std::end(kRopeActions); }
// While holding, 7CC69D61 / 563271FE repeat every ~1.4 s: none for this long
// and the sender is no longer on the rope.
constexpr uint64_t kRopeWatchdogMs = 5000;

// Read-only look at the copy's rope slot 0 (and ours) after a hook, to see
// how far its rope got (arbol-cuerda-2.md fix 3). comp = [[actor+0x68]+0x78].
std::atomic<uint64_t> g_rope_probe_at{0};
std::atomic<int> g_rope_probes{0};
// [dev] rope_piece_lookup: the tree's piece id on this PC, from its scene id,
// the way the game finds it (0x5947C7): without a piece the rope has no body
// to pull and the tree cannot bend.
std::atomic<bool> g_rope_lookup{false};
uint64_t g_rope_lookups = 0, g_rope_lookup_found = 0;
// The tree bends for the one pulling only: each frame the game calls
// TreeComponent::SetBend for the LOCAL player (0x1190FD0, input-driven), never
// for the copy (gate 0x86A557), and no message carries it (arbol-cuerda-3.md).
// [dev] rope_bend: a mid-hook in the same actor update (0x86A576) calls it for
// the copy while the sender pulls (its pull actions), visual only.
std::atomic<bool> g_rope_bend{false};
std::atomic<bool> g_copy_pulling{false};      // the sender's last main action is a pull one (network thread)
std::atomic<uintptr_t> g_copy_actor{0}, g_own_actor{0};  // refreshed by puppet_tick
std::atomic<float> g_pull_seconds{0.0f};
std::atomic<uint64_t> g_bends{0};
constexpr uint32_t kRopePullActions[] = {0xD9C333C4, 0x76D8C2D8, 0x6B4EE3CA, 0x563271FE};
constexpr float kBendPerSecond = 0.15f, kBendMax = 0.8f;
SafetyHookMid g_bend_hook;
// The rope's effect state (arbol-cuerda-4-cuerda-visible.md R1): only the
// frame event ClientFrameEventRemoteCatchEffectState (0x595FE0) turns on
// u8 comp+0xAC0 / f32 comp+0xABC per slot (stride 0x1A0), and it never runs
// on the copy: its rope's shader ratio stays 0 and only part of it shows.
// The same actor-update hook sets them while the copy is on a tree (phase
// 2-3), visual only (read by 0x11930A0 alone). Off for good at a fault.
std::atomic<bool> g_rope_effect{true};
uintptr_t g_effect_actor = 0;  // the copy whose effect is on (actor-update thread)
// R2: the pull's flex on the copy's rope (+0x4E0 force, +0x4E4 speed,
// +0x4E8 dir, u8 +0x4F4 on), as the local player's pull writes it.
std::atomic<uintptr_t> g_flex_comp{0}, g_flex_actor{0};  // set on the copy's own update thread
// The rope's far end (arbol-cuerda-5.md R5): the drawn rope is [node A, ...,
// node B] (0x5AEAA0, every frame); A and B hang from bones of the rope tool,
// which only the local player has and moves onto the hooked point
// (0x17A6210, local only). The copy has no tool: its B stayed by its body
// and the rope came out short, pointing elsewhere. A mid-hook in the
// per-slot tick, right before 0x5AEAA0 (0x59D89A), puts B on the hook point
// each frame, as 0x5AEAA0 places the tip. Also measures |B - target| for
// the copy and for us (the probe).
SafetyHookMid g_rope_end_hook;
std::atomic<bool> g_rope_end{false};
std::atomic<uint64_t> g_rope_end_sets{0};
std::atomic<float> g_end_gap_copy{-1.0f}, g_end_gap_ours{-1.0f};  // |B - target| before our write, metres
// Set while the game's lookup runs; still set after it = it faulted.
bool g_rope_lookup_faulted = false;
// ReleaseRemoteCatchResultAck: u32 catcher | u8 slot | u8 reason (reader
// 0xB5BD60 -> 0x5A7A10; the game's own releases use reason 2).
constexpr uint16_t kOpReleaseRemoteCatchResultAck = 2471;
constexpr uint8_t kReleaseReason = 2;

std::string scene_text(const SceneId& id) {
    std::string s;
    for (size_t i = 0; i < 8; ++i) s += std::format("{:02X}", id[7 - i]);
    return s;
}

Bytes rope_release() {
    g_copy_pulling = false;
    Bytes b(6);
    std::memcpy(b.data(), &kPuppetPlayer, 4);
    b[4] = g_hook_slot;
    b[5] = kReleaseReason;
    return make_packet(kOpReleaseRemoteCatchResultAck, b);
}

// Network thread: the other player's RemoteCatchResultAck on a tree, made
// to name the tree as our client knows it.
bool hook_tree_here(Bytes& body) {
    if (body.size() < kRemoteCatchPoint + 12) return false;
    SceneId id;
    std::memcpy(id.data(), body.data() + off::kRemoteCatchSceneId, id.size());
    bool here = false;
    uint64_t piece = ~0ull;  // -1: the tree by its scene id, the point relative to it
    {
        std::lock_guard lock(g_gimmick_mutex);
        here = g_scene_gimmicks.count(id) != 0;
        if (auto p = g_own_pieces.find(id); p != g_own_pieces.end()) piece = p->second;
    }
    float point[3];
    std::memcpy(point, body.data() + kRemoteCatchPoint, sizeof point);
    if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])) return false;
    std::memcpy(body.data() + kRemoteCatchPiece, &piece, 8);
    // A point in world coordinates (a static body): no scene object to add.
    if (std::fabs(point[0]) > 1000.0f || std::fabs(point[2]) > 1000.0f) {
        std::memset(body.data() + off::kRemoteCatchSceneId, 0, 16);
    }
    if (!here) {
        if (g_hooks_not_here++ < 20) {
            log::info("Puppet: the other player hooked tree {} with the rope, but it is not loaded here; not shown",
                      scene_text(id));
        }
        return false;
    }
    if (g_hooks_shown++ < 50) {
        log::info("Puppet: the other player hooked tree {} with the rope ({})", scene_text(id),
                  piece == ~0ull ? std::string("located by its scene id")
                                 : std::format("our piece 0x{:016X} of it", piece));
    }
    return true;
}

Bytes g_last_move;              // last Move (Req format, puppet key)
uint64_t g_last_move_ms = 0;    // steady_ms when it was processed
uint64_t g_last_move_ts = 0;    // its timestamp (sender clock)
float g_last_move_pos[3] = {NAN, NAN, NAN};
constexpr uint64_t kMoveSilenceMs = 150;  // real MoveActorReqs come every ~25 ms

// When things go in. Timestamps are moved by the playout delay D (coop
// delay_ms) and the client plays positions from its buffer 200 ms behind its
// clock, but it starts an action (StartAction, a turn, a weapon change) the
// moment it receives it.
// - Moves are held only kHoldMs after arriving: early samples wait in the
//   client's buffer, so late network packets still make it (margin
//   D + 200 - hold instead of ~200 - lateness; saltos-copia-anfitrion.md P1).
// - Actions go in when the buffer reaches their timestamp, so the copy
//   starts climbing at the foot of the obstacle and not metres before it
//   (live test 6 Oct: with every action 400 ms early the copy climbed early
//   and then popped onto the ledge). They also stay queued while the sender
//   is frozen, so the Move that resumes can still re-path them
//   (fill_gap_before).
// Each kind goes in in arrival order: a due time never goes below the previous one.
constexpr uint64_t kHoldMs = 100;
constexpr int64_t kClientRenderDelayMs = 200;  // exe+0x6CB9548 (build 2976)
uint64_t g_last_due = 0, g_last_action_due = 0;
uint64_t g_action_hold = 0;  // the latest action's hold: for WallUp and weapon changes
// The copy was reloaded: the Move before it is no reference for gap filling.
std::atomic<bool> g_gap_ref_stale{false};

uint64_t movement_due(uint64_t due) {
    if (due < g_last_due) due = g_last_due;
    g_last_due = due;
    return due;
}
uint64_t action_due(uint64_t due) {
    if (due < g_last_action_due) due = g_last_action_due;
    g_last_action_due = due;
    return due;
}
uint64_t move_hold_for(uint64_t delay_ms) { return std::min(delay_ms, kHoldMs); }
// Until the client's buffer reaches the action's (shifted) timestamp at ack+ts_at.
uint64_t action_hold_for(const Bytes& ack, size_t ts_at, uint64_t delay_ms) {
    if (!capture_game_clock_trusted() || ack.size() < ts_at + 8) return delay_ms;
    const uint64_t game_now = capture_game_clock_ms();
    uint64_t ts;
    std::memcpy(&ts, ack.data() + ts_at, 8);
    const int64_t wait = static_cast<int64_t>(ts) + kClientRenderDelayMs - static_cast<int64_t>(game_now);
    return static_cast<uint64_t>(std::clamp<int64_t>(wait, 0, static_cast<int64_t>(delay_ms) + kClientRenderDelayMs));
}

// Gap filling. During a ledge climb-up / vault the sender sends no Move for
// 0.3-1.5 s and even its physics position is frozen; the first Move after the
// gap jumps 1-3 m. The puppet's buffer resets after 1 s without useful samples
// and then covers the jump in ~200 ms (a visible pop). Because events are
// delayed, we can still insert the missing samples behind the jump before
// they are due: a path that goes up first and then forward (forward then
// down when dropping). Fully smooth only if the delay exceeds the gap.
constexpr uint64_t kGapMinMs = 150;
constexpr uint64_t kGapMaxMs = 3000;
constexpr float kGapMinJump = 0.5f;   // metres
constexpr float kGapMaxJump = 15.0f;  // beyond this it's a real teleport (fast travel)
constexpr uint64_t kGapStepMs = 50;
uint64_t g_gap_fills = 0;

struct RecentGap {
    bool valid;
    uint64_t detected_ms;  // steady_ms when the resuming Move was seen
    float start[3];        // frozen position
    float end[3];          // position after the climb-up
};
RecentGap g_recent_gap{};

void fill_gap_before(const Bytes& next, uint64_t delay) {
    if (g_gap_ref_stale.exchange(false)) return;  // reloaded since that Move (5 Oct: 1-7 m jumps)
    if (g_last_move.empty()) return;
    uint64_t ts_next = 0;
    std::memcpy(&ts_next, next.data() + 0xD5, 8);
    if (ts_next <= g_last_move_ts) return;
    const uint64_t gap = ts_next - g_last_move_ts;
    if (gap < kGapMinMs || gap > kGapMaxMs) return;
    float a[3], b[3];
    std::memcpy(a, g_last_move.data() + 0x04, sizeof a);
    std::memcpy(b, next.data() + 0x04, sizeof b);
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < kGapMinJump || dist > kGapMaxJump) return;

    const bool rising = dy >= 0;
    // Point of the climb path at fraction f (0 = before the gap, 1 = after).
    // Rising: vertical in the first 60 %, then horizontal. Falling: the reverse.
    auto path_at = [&](float f, float out[3]) {
        const float split = 0.6f;
        float fv, fh;
        if (rising) {
            fv = std::min(f / split, 1.0f);
            fh = f <= split ? 0.0f : (f - split) / (1.0f - split);
        } else {
            fh = std::min(f / split, 1.0f);
            fv = f <= split ? 0.0f : (f - split) / (1.0f - split);
        }
        out[0] = a[0] + dx * fh;
        out[1] = a[1] + dy * fv;
        out[2] = a[2] + dz * fh;
    };

    // StartActions sent during (and right at the end of) the freeze carry the
    // frozen START position and feed the same position buffer as Moves, so they
    // dragged the puppet back to the foot of the ledge after our samples lifted
    // it (captura 134008). They are still in the delay queue: move their
    // positions onto the path at their own timestamp.
    const int64_t shift = g_ts_shift;
    const uint64_t win_lo = static_cast<uint64_t>(static_cast<int64_t>(g_last_move_ts) + shift);
    // Up to 300 ms past the gap: StartActions right after the resuming Move
    // can still carry the frozen start position; those go to the top.
    const uint64_t win_hi = static_cast<uint64_t>(static_cast<int64_t>(ts_next) + shift) + 300;
    int patched = 0;
    AcquireSRWLockExclusive(&g_queue_lock);
    for (Pending& item : g_queue) {
        Bytes& pk = item.packet;
        if (!item.movement || pk.size() != 5 + 725) continue;
        uint16_t op;
        std::memcpy(&op, pk.data(), 2);
        if (op != off::kOpStartActionAck) continue;
        uint64_t ts;
        std::memcpy(&ts, pk.data() + 5 + 0x160, 8);
        if (ts < win_lo || ts > win_hi) continue;
        float p[3];
        path_at(std::min(1.0f, static_cast<float>(ts - win_lo) / static_cast<float>(gap)), p);
        p[0] += g_offset_x;
        for (size_t at : {size_t{0x8F}, size_t{0xD7}}) {
            float v[3];
            std::memcpy(v, pk.data() + 5 + at, sizeof v);
            const float ex = v[0] - (a[0] + g_offset_x), ey = v[1] - a[1], ez = v[2] - a[2];
            if (ex * ex + ey * ey + ez * ez < 4.0f) {  // only the frozen start position
                std::memcpy(pk.data() + 5 + at, p, sizeof p);
                ++patched;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_queue_lock);

    // StartActions arriving shortly AFTER the resuming Move (not queued yet)
    // get fixed on arrival, see puppet_event.
    g_recent_gap = {true, steady_ms(), {a[0], a[1], a[2]}, {b[0], b[1], b[2]}};

    const uint64_t steps = gap / kGapStepMs;
    for (uint64_t k = 1; k < steps; ++k) {
        const float f = static_cast<float>(k) / static_cast<float>(steps);
        float p[3];
        path_at(f, p);
        Bytes req = g_last_move;
        std::memcpy(req.data() + 0x04, p, sizeof p);
        std::memcpy(req.data() + 0x4C, p, sizeof p);
        const uint64_t ts = g_last_move_ts + k * kGapStepMs;
        std::memcpy(req.data() + 0xD5, &ts, 8);
        auto ack = move_ack(req);
        if (!ack) return;
        // Due when the original sample would have been: received at the time
        // matching its timestamp, plus the hold. Overdue ones go in at once;
        // the client plays them by timestamp.
        const uint64_t due = g_last_move_ms + k * kGapStepMs + delay;
        enqueue_at(make_packet(off::kOpMoveActorAck, *ack), movement_due(due), /*movement=*/true);
    }
    if (g_gap_fills++ < 200) {  // rare events: log them all
        log::info("Puppet: filled a {} ms gap ({:.2f} m, {}) with {} samples, re-pathed {} "
                  "queued StartAction positions [{} gaps so far]",
                  gap, dist, rising ? "up" : "down", steps - 1, patched, g_gap_fills);
    }
}

// ---------------------------------------------------------------- free wall climb
// Free wall climb (movement method 18, PointClimb). Each StartAction stores
// the method's Enter() result at method+0x44, and while it is 0 the per-frame
// code skips both the network interpolation and the SetPosition call. Enter
// needs a climb point that only the local character's sensor computes, so on
// the puppet it always failed: the puppet hung at the grab point for the
// whole climb (measured live: its network buffer and TS+0x534 kept rising,
// its transform and physics stayed frozen) and popped ~10 m at the top. For
// the puppet only, report success: in control mode 2 the flag just enables
// the interpolation, exactly as for walking (docs/research/pointclimb-remoto.md).
SafetyHookInline g_climb_enter_hook;
std::atomic<uint64_t> g_climb_forced{0};
std::atomic<uint64_t> g_climb_other_fails{0};
std::atomic<uint32_t> g_climb_last_owner{0};
uint64_t g_climb_forced_logged = 0;
uint64_t g_climb_other_logged = 0;

uint32_t owner_of_method(void* method) {
    __try {
        const auto actor = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(method) +
                                                               off::kMethod_Actor);
        return actor ? *reinterpret_cast<const uint32_t*>(actor + off::kActor_OwnerUser) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Mounting and dismounting (RideOn/RideOff, methods 6/7): same pattern, their
// Enter needs the rider<->horse link that a remote copy never gets.
SafetyHookMid g_layer_guard_hook;
uint8_t* g_ret_stub = nullptr;  // a lone `ret`: resume there to return at once
std::atomic<uint64_t> g_layer_guard_fired{0};
std::atomic<uintptr_t> g_layer_guard_caller{0};
uint64_t g_layer_guard_logged = 0;

bool layer_has_no_playback(uintptr_t slot) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(slot) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void zero_float_out(uintptr_t rsp) {
    __try {
        if (auto* out = *reinterpret_cast<float**>(rsp + 0x28)) *out = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void action_layer_advance(safetyhook::Context& ctx) {
    if (!layer_has_no_playback(ctx.rcx)) return;
    // Nothing is playing on this layer: advance nothing, like the caller
    // does for a rate <= 0. rsp still points at the return address.
    zero_float_out(ctx.rsp);
    std::memset(&ctx.xmm0, 0, sizeof ctx.xmm0);
    __try {
        g_layer_guard_caller.store(*reinterpret_cast<const uintptr_t*>(ctx.rsp), std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_layer_guard_fired.fetch_add(1, std::memory_order_relaxed);
    ctx.rip = reinterpret_cast<uintptr_t>(g_ret_stub);
}

// One level up: skip the whole layer update of one of OUR copies while a
// layer has a chart but nothing playing (until its first action applies).
SafetyHookMid g_layers_skip_hook;
std::atomic<uint64_t> g_layers_skipped{0};
std::atomic<uint32_t> g_layers_skipped_key{0};
uint64_t g_layers_skipped_logged = 0;

// 0 = fine; otherwise the actor key of a copy whose layer has no action.
uint32_t puppet_layer_without_action(uintptr_t control) {
    __try {
        const auto actor = *reinterpret_cast<const uintptr_t*>(control + 0x08);
        if (!actor) return 0;
        const uint32_t key = *reinterpret_cast<const uint32_t*>(actor + off::kActor_Key);
        if (!is_puppet_key(key) && !is_enemy_copy_key(key)) return 0;
        for (uint32_t i = 0; i < 2; ++i) {
            const auto layer = *reinterpret_cast<const uintptr_t*>(control + off::kControl_Layer0 + 8 * i);
            if (!layer) continue;
            if (*reinterpret_cast<const uintptr_t*>(layer + off::kLayer_Chart) != 0 &&
                *reinterpret_cast<const uintptr_t*>(layer + off::kLayer_Playback) == 0) {
                return key;
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void action_layers_advance(safetyhook::Context& ctx) {
    const uint32_t key = puppet_layer_without_action(ctx.rcx);
    if (!key) return;
    g_layers_skipped_key.store(key, std::memory_order_relaxed);
    g_layers_skipped.fetch_add(1, std::memory_order_relaxed);
    ctx.rip = reinterpret_cast<uintptr_t>(g_ret_stub);  // return at once
}

SafetyHookInline g_ride_on_hook;
SafetyHookInline g_ride_off_hook;
std::atomic<uint64_t> g_ride_forced{0};

uint8_t force_for_puppet(uint8_t ok, void* method, std::atomic<uint64_t>& forced) {
    if (ok) return ok;
    if (owner_of_method(method) == kPuppetUser) {
        forced.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return ok;
}

uint8_t ride_on_enter(void* method, uint8_t same_method) {
    return force_for_puppet(g_ride_on_hook.unsafe_call<uint8_t>(method, same_method), method, g_ride_forced);
}

// Not forced: forcing RideOff::Enter left the copy stuck in the dismount
// pose for good (live test 1). Unforced, the dismount freezes briefly and the
// next action recovers.
uint8_t ride_off_enter(void* method, uint8_t same_method) {
    return g_ride_off_hook.unsafe_call<uint8_t>(method, same_method);
}

uint8_t point_climb_enter(void* method, uint8_t same_method) {
    const uint8_t ok = g_climb_enter_hook.unsafe_call<uint8_t>(method, same_method);
    if (ok) return ok;
    const uint32_t owner = owner_of_method(method);
    if (owner == kPuppetUser) {
        g_climb_forced.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    g_climb_last_owner.store(owner, std::memory_order_relaxed);
    g_climb_other_fails.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

}  // namespace

// The horse's load packet carries its position three times (caballo-coop.md
// §2.2); the last Transform is found from its end marker: scale (1,1,1)
// followed by an all-FF parent. Every copy of that position is moved.
bool spawn_horse(const proto::SpawnInfo& info, uint64_t now) {
    Bytes horse = info.horse;
    if (horse.size() < 0x40 || !keys::is_class(info.horse_key, keys::kClassWorld) || is_puppet_key(info.horse_key)) {
        return false;
    }
    replace_u32(horse, info.user_key, kPuppetUser);
    replace_u32(horse, info.player_key, kPuppetPlayer);
    replace_u32(horse, info.horse_key, kPuppetHorse);
    // The copy's user, then its mount in one of the user's mount slots,
    // nothing of ours (security review H1; layout checked on the captures of
    // 5-6 Oct). The horse "Herspia" goes in slot 2, the mount "Rokade" in slot
    // 3 (two-PC test, 6 Oct); the game bounds the slot itself (100 slots,
    // caballo-coop.md §8.4).
    const auto chain = record_chain(horse);
    const bool mount_slot = chain && chain->size() == 2 && (*chain)[1].index >= kFirstMountSlot &&
                            (*chain)[1].index <= kLastMountSlot;
    if (!chain || chain->size() != 2 || (*chain)[0].type != 0 || (*chain)[0].key != kPuppetUser || !mount_slot ||
        !head_is((*chain)[1], 5, kPuppetHorse, kPuppetUser, (*chain)[1].index) || names_our_player(horse)) {
        log::warn("Puppet: the other player's horse is not a plain mount load{}; not loaded",
                  chain && chain->size() == 2 ? std::format(" (slot {})", (*chain)[1].index) : std::string());
        return false;
    }
    // Note: loading it without an owner record (+0x12 = 0, caballo-coop.md §8)
    // crashed the game right after the load (live test 2, access violation at
    // exe+0x1F63FE6 on another thread). With the owner it is silently not
    // created; the horse stays experimental and off by default.
    // Party list: ... | player | other member | horse | ... The other member
    // is an actor of the sender's world; give it a key of ours.
    for (size_t i = 0; i + 12 <= horse.size(); ++i) {
        uint32_t a, b, c;
        std::memcpy(&a, horse.data() + i, 4);
        std::memcpy(&b, horse.data() + i + 4, 4);
        std::memcpy(&c, horse.data() + i + 8, 4);
        if (a == kPuppetPlayer && c == kPuppetHorse && keys::is_class(b, keys::kClassWorld) && !is_puppet_key(b)) {
            std::memcpy(horse.data() + i + 4, &kPuppetHorseParty, 4);
        }
    }
    static const uint8_t kMarker[20] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
                                        0x80, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    size_t marker = std::string::npos;
    for (size_t i = horse.size() - sizeof kMarker; i > 28; --i) {
        if (std::memcmp(horse.data() + i, kMarker, sizeof kMarker) == 0) {
            marker = i;
            break;
        }
    }
    if (marker == std::string::npos) {
        log::warn("Puppet: horse packet has no Transform marker; horse not spawned");
        return false;
    }
    uint8_t old_pos[12];
    std::memcpy(old_pos, horse.data() + marker - 28, sizeof old_pos);
    const float pos[3] = {info.horse_pos.x + g_offset_x, info.horse_pos.y, info.horse_pos.z};
    int copies = 0;
    for (size_t i = 0; i + 12 <= horse.size(); ++i) {
        if (std::memcmp(horse.data() + i, old_pos, 12) == 0) {
            std::memcpy(horse.data() + i, pos, 12);
            ++copies;
            i += 11;
        }
    }
    // At least 3 s after the player's packets (the copy's user must be logged
    // in first), and only together with the horse's first action (horse_event).
    {
        std::lock_guard lock(g_mutex);
        g_horse_pending = make_packet(off::kOpActorLoadDataAck, horse);
        g_horse_not_before = now + 3000;
    }
    horse_diag_watch(info.horse_key);
    log::info("Puppet: horse prepared at ({:.1f}, {:.1f}, {:.1f}) ({} bytes, position x{}, sender "
              "horse 0x{:08X}); loaded with its first action",
              pos[0], pos[1], pos[2], horse.size(), copies, info.horse_key);
    return true;
}

bool puppet_spawn(const proto::SpawnInfo& info) {
    g_copy_hooked = false;  // a new copy holds no rope
    Bytes user = info.user, body = info.body;
    if (!keys::is_class(info.user_key, keys::kClassUser) || !keys::is_class(info.player_key, keys::kClassPlayer) ||
        !keys::is_class(info.body_key, keys::kClassWorld) || is_puppet_key(info.user_key) ||
        is_puppet_key(info.player_key) || is_puppet_key(info.body_key)) {
        log::warn("Puppet: the other player's character names impossible keys; not loaded");
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_spawned) {
            log::warn("Puppet: already spawned; remove it first");
            return false;
        }
        for (auto* v : {&user, &body}) {
            const int a = replace_u32(*v, info.user_key, kPuppetUser);
            const int b = replace_u32(*v, info.player_key, kPuppetPlayer);
            const int c = replace_u32(*v, info.body_key, kPuppetBody);
            log::info("Puppet: rewrote keys in {} packet: user x{}, player x{}, body x{}",
                      v == &user ? "user" : "body", a, b, c);
        }
        g_spawned = true;
        g_sender_riding = false;
        g_unmounted_moves = 0;
        g_src_player_key = info.player_key;
        g_src_user_key = info.user_key;
        g_sender_horse = info.horse_key;
        g_src_horse_key = 0;  // set once the horse load is queued
        g_horse_spawned = false;
    }

    // Both packets carry the world position (3 floats) 154 bytes before the
    // end. Changing only the body's copy had no effect in game (the copy kept
    // appearing at the load spot), so write both.
    // At user +0x17 and +0x43; the body embeds a copy of the user record
    // (found by its header). Other fields can hold the same max value, so only
    // these positions are touched.
    if (!set_character_no(user, body)) {
        // Sharing our player's number mixes up our own equipment (live test 1).
        std::lock_guard lock(g_mutex);
        g_spawned = false;
        log::warn("Puppet: character number not where expected; not loaded");
        return false;
    }
    if (user.size() < 256 || body.size() < 256) {
        std::lock_guard lock(g_mutex);
        g_spawned = false;
        log::warn("Puppet: spawn data too short; ignored");
        return false;
    }
    if (std::fabs(info.pos.x) > proto::kMaxWorldCoord || std::fabs(info.pos.z) > proto::kMaxWorldCoord ||
        std::fabs(info.pos.y) > proto::kMaxWorldHeight) {
        std::lock_guard lock(g_mutex);
        g_spawned = false;
        log::warn("Puppet: the other player's character is outside the world; not loaded");
        return false;
    }
    const float pos[3] = {info.pos.x + g_offset_x, info.pos.y, info.pos.z};
    const float rot[4] = {info.rot.x, info.rot.y, info.rot.z, info.rot.w};
    const bool rot_ok = std::fabs(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3] - 1.0f) < 0.05f;
    for (auto* v : {&user, &body}) {
        const size_t pos_off = v->size() - off::kLoadPositionFromEnd;
        std::memcpy(v->data() + pos_off, pos, sizeof pos);
        // Facing: the quaternion right after the position (Transform).
        if (rot_ok) std::memcpy(v->data() + v->size() - off::kLoadRotationFromEnd, rot, sizeof rot);
    }
    // The body packet is [u16 len][record]... : its own record, then a byte
    // copy of the user packet. For the real player the client skips the
    // repeated player record (it exists, owned by the local user); for the
    // copy it tries to create 0xA010F001 again and logs "Actor creation
    // failed" (caballo-coop.md §9.7). Send only the body's own record.
    {
        size_t at = 0;
        int records = 0;
        uint16_t first = 0;
        while (at + 2 <= body.size()) {
            uint16_t len = 0;
            std::memcpy(&len, body.data() + at, 2);
            if (records == 0) first = len;
            at += 2 + len;
            ++records;
        }
        if (at == body.size() && records >= 2 && first >= 0x40) {
            body.resize(2 + first);
        } else {
            std::lock_guard lock(g_mutex);
            g_spawned = false;
            log::warn("Puppet: body packet layout not as expected ({} records); not loaded", records);
            return false;
        }
    }
    {
        const auto u = record_chain(user);
        const auto b = record_chain(body);
        const bool ok = u && b && u->size() == 2 && b->size() == 1 && (*u)[0].type == 0 &&
                        (*u)[0].key == kPuppetUser && head_is((*u)[1], 1, kPuppetPlayer, kPuppetUser, 0) &&
                        head_is((*b)[0], 7, kPuppetBody, 0, 0xFF) && !names_our_player(user) &&
                        !names_our_player(body);
        if (!ok) {
            std::lock_guard lock(g_mutex);
            g_spawned = false;
            log::warn("Puppet: the other player's character is not a plain player load (records {}/{}); "
                      "not loaded",
                      u ? static_cast<int>(u->size()) : -1, b ? static_cast<int>(b->size()) : -1);
            return false;
        }
    }
    const uint64_t now = steady_ms();
    enqueue_at(make_packet(off::kOpActorLoadDataAck, user), now, /*movement=*/false);
    enqueue_at(make_packet(off::kOpActorLoadDataAck, body), now, /*movement=*/false);
    bool reuse = false;
    {
        std::lock_guard lock(g_mutex);
        reuse = g_horse_kept && info.horse_key == g_sender_horse && game::find_actor(kPuppetHorse);
        g_horse_kept = false;
        if (reuse) {
            g_horse_spawned = true;
            g_src_horse_key = info.horse_key;
        }
    }
    if (reuse) {
        log::info("Puppet: rider reloaded next to the horse copy that stayed");
    } else if (g_horse_enabled && !info.horse.empty() && info.horse_key && spawn_horse(info, now)) {
        std::lock_guard lock(g_mutex);
        g_horse_spawned = true;
        g_src_horse_key = info.horse_key;
    }
    log::info("Puppet: spawn queued at ({:.1f}, {:.1f}, {:.1f}) (2 packets, {} + {} bytes, "
              "sender player 0x{:08X}, generation {})",
              pos[0], pos[1], pos[2], user.size(), body.size(), info.player_key, info.generation);
    // Gear the sender changed since its templates were captured. The copy
    // loads wearing the template's gear, and the client drops a whole put
    // list if one item lands on an occupied slot: first empty every slot the
    // template had filled (OtherUnequipItemAck, no serial gate), then put the
    // items on, as the live messages left them (ropa-al-desmontar.md section
    // 5.2). Not movement: a purge or a late delivery must not lose them.
    if (g_equip_enabled && EquipState::plausible(info.equip.data(), info.equip.size())) {
        EquipState state;
        state.apply(info.equip);
        const auto removals = state.removals();
        const auto puts = state.puts();
        const uint64_t at = now + 1000;
        for (uint16_t slot : removals) {
            enqueue_at(make_packet(off::kOpOtherUnequipItemAck,
                                   unequip_payload(kPuppetPlayer, slot, next_equip_serial(state.serial()))),
                       at, /*movement=*/false);
        }
        uint32_t serial = 0;
        if (!puts.empty()) {
            serial = next_equip_serial(state.serial());
            enqueue_at(make_packet(off::kOpOtherEquipItemAck, EquipState::payload(kPuppetPlayer, serial, puts)), at,
                       /*movement=*/false);
        }
        g_equip_check_at = at + 1500;
        log::info("Puppet: gear changed since the sender's load, applied after spawn: {} slot(s) emptied, "
                  "{} item(s) put (serial {})",
                  removals.size(), puts.size(), serial);
    }
    return true;
}

bool puppet_remove() {
    {
        std::lock_guard lock(g_mutex);
        if (!g_spawned) return false;
        g_spawned = false;
    }
    purge_movement();  // no movement for an actor that is about to disappear
    g_gap_ref_stale = true;
    // RemoveActorAck payload: 04 00 00 00 00 | type | u32 key. The type byte
    // varies per actor kind and is not understood yet; 0x15 is the most common.
    bool horse = false;
    {
        std::lock_guard lock(g_mutex);
        horse = g_horse_spawned;
        g_horse_spawned = false;
    }
    bool kept = false;
    {
        std::lock_guard lock(g_mutex);
        if (!g_horse_pending.empty()) horse = false;  // never loaded
        g_horse_pending.clear();
        if (g_horse_kept) horse = true;  // left without its rider earlier
        if (horse && g_keep_horse) {
            horse = false;
            kept = g_horse_kept = true;
        } else {
            g_horse_kept = false;
        }
        g_keep_horse = false;
    }
    if (kept) log::info("Puppet: the horse copy stays while its rider reloads");
    if (horse) {
        Bytes payload = {0x04, 0x00, 0x00, 0x00, 0x00, 0x15, 0, 0, 0, 0};
        std::memcpy(payload.data() + 6, &kPuppetHorse, 4);
        enqueue_at(make_packet(off::kOpRemoveActorAck, payload), steady_ms(), /*movement=*/false);
    }
    for (uint32_t key : {kPuppetBody, kPuppetPlayer}) {
        Bytes payload = {0x04, 0x00, 0x00, 0x00, 0x00, 0x15, 0, 0, 0, 0};
        std::memcpy(payload.data() + 6, &key, 4);
        enqueue_at(make_packet(off::kOpRemoveActorAck, payload), steady_ms(), /*movement=*/false);
    }
    log::info("Puppet: remove queued (body 0x{:08X}, player 0x{:08X})", kPuppetBody, kPuppetPlayer);
    return true;
}

void puppet_forget() {
    g_copy_hooked = false;
    {
        std::lock_guard lock(g_mutex);
        if (!g_spawned) return;
        g_spawned = false;
        g_horse_spawned = false;
        g_horse_pending.clear();
        g_horse_kept = g_keep_horse = false;
    }
    purge_movement();
    g_gap_ref_stale = true;
    AcquireSRWLockExclusive(&g_queue_lock);
    g_inject_clock.clear();  // no RemoveActorAck went in for it
    ReleaseSRWLockExclusive(&g_queue_lock);
    log::info("Puppet: forgotten (no longer in the game's actor list)");
}

bool puppet_take_reload_request() { return g_reload_request.exchange(false); }
void puppet_request_reload() { g_reload_request = true; }
bool puppet_spawn_enemy(uint32_t copy_key, std::vector<uint8_t> load, std::vector<uint8_t> action_req) {
    // Every copy of its position moves by the puppet offset (found from the
    // last Transform, as for the horse).
    static const uint8_t kMarker[20] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
                                        0x80, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int copies = 0;
    float pos[3] = {NAN, NAN, NAN};
    float orig[3] = {NAN, NAN, NAN};
    float rot[4] = {0, 0, 0, 1};
    for (size_t i = load.size() - sizeof kMarker; i > 28; --i) {
        if (std::memcmp(load.data() + i, kMarker, sizeof kMarker) != 0) continue;
        uint8_t old_pos[12];
        std::memcpy(old_pos, load.data() + i - 28, sizeof old_pos);
        std::memcpy(orig, old_pos, sizeof orig);
        std::memcpy(rot, load.data() + i - 16, sizeof rot);
        std::memcpy(pos, old_pos, sizeof pos);
        pos[0] += g_offset_x;
        for (size_t j = 0; j + 12 <= load.size(); ++j) {
            if (std::memcmp(load.data() + j, old_pos, 12) == 0) {
                std::memcpy(load.data() + j, pos, 12);
                ++copies;
                j += 11;
            }
        }
        break;
    }
    const uint64_t now = steady_ms();
    enqueue_at(make_packet(off::kOpActorLoadDataAck, load), now, /*movement=*/false);
    // Its recent actions, oldest first, right behind the load (one per action
    // layer at least; see enemy_relay.cpp).
    // They are its pose, not movement: all stamped "now" on this client's game
    // clock. Their recorded times are the sender's, up to minutes old or ahead
    // of ours, and the client held the copy's samples until then (copies drawn
    // between the origin and their place, enemigos-faltan-invitado-7oct.md §4).
    std::optional<uint64_t> game_now = capture_game_clock_trusted() ? game::client_game_clock_ms() : std::nullopt;
    if (!game_now) {
        if (const uint64_t est = capture_game_clock_ms()) game_now = est;  // the estimate (clock read off)
    }
    int with_action = 0;
    for (size_t at = 0; at + 725 <= action_req.size(); at += 725) {
        const Bytes one(action_req.begin() + at, action_req.begin() + at + 725);
        if (auto ack = start_action_ack(one)) {
            if (game_now && ack->size() >= 0x168) std::memcpy(ack->data() + 0x160, &*game_now, 8);
            enqueue_at(make_packet(off::kOpStartActionAck, *ack), now, /*movement=*/false);
            ++with_action;
        }
    }
    // A network-driven actor is placed (and drawn) from its movement samples:
    // without one the first copies loaded but never showed (live test). One
    // still sample where it stands, built on the other player's last Move.
    bool with_move = false;
    if (g_last_move.size() == 514 && std::isfinite(orig[0])) {
        Bytes m = g_last_move;
        std::memcpy(m.data() + 0x00, &copy_key, 4);
        std::memcpy(m.data() + 0x04, orig, 12);
        std::memcpy(m.data() + 0x10, rot, 16);
        std::memset(m.data() + 0x20, 0, 4);
        std::memcpy(m.data() + 0x4C, orig, 12);  // Path.local = world, no parent
        std::memcpy(m.data() + 0x58, rot, 16);
        const float one[3] = {1.0f, 1.0f, 1.0f};
        std::memcpy(m.data() + 0x68, one, sizeof one);
        std::memset(m.data() + 0x8C, 0xFF, 8);
        std::memset(m.data() + 0x94, 0, 0x40);
        float ref[3];
        std::memcpy(ref, g_ref_pos, sizeof ref);  // the sender's reference stays
        auto ack = move_ack(m);
        std::memcpy(g_ref_pos, ref, sizeof ref);
        if (ack) {
            enqueue_at(make_packet(off::kOpMoveActorAck, *ack), now + 200, /*movement=*/false);
            with_move = true;
        }
    }
    log::info("Enemy relay: copy 0x{:08X} queued at ({:.1f}, {:.1f}, {:.1f}) ({} B, position x{}, {}, {})",
              copy_key, pos[0], pos[1], pos[2], load.size(), copies,
              with_action ? std::to_string(with_action) + " actions" : std::string("NO action"),
              with_move ? "placed by a Move" : "NO move");
    return true;
}

// Each enemy copy's latest (unshifted) position: its own actions carry
// positions near IT, which shift_x_if_near_sender must recognise.
std::map<uint32_t, std::array<float, 3>> g_enemy_pos;

void puppet_enemy_move(uint32_t copy_key, uint64_t ts, const proto::Vec3& pos, const proto::Quat& rot,
                       int64_t ts_shift_ms, uint64_t delay_ms) {
    if (g_last_move.size() != 514) return;
    Bytes m = g_last_move;
    const float p[3] = {pos.x, pos.y, pos.z};
    const float q[4] = {rot.x, rot.y, rot.z, rot.w};
    std::memcpy(m.data() + 0x00, &copy_key, 4);
    std::memcpy(m.data() + 0x04, p, sizeof p);
    std::memcpy(m.data() + 0x10, q, sizeof q);
    std::memset(m.data() + 0x20, 0, 4);
    std::memcpy(m.data() + 0x4C, p, sizeof p);  // Path.local = world, no parent
    std::memcpy(m.data() + 0x58, q, sizeof q);
    const float one[3] = {1.0f, 1.0f, 1.0f};
    std::memcpy(m.data() + 0x68, one, sizeof one);
    std::memset(m.data() + 0x8C, 0xFF, 8);
    std::memset(m.data() + 0x94, 0, 0x40);
    std::memcpy(m.data() + 0xD5, &ts, 8);
    g_enemy_pos[copy_key] = {p[0], p[1], p[2]};
    float ref[3];
    std::memcpy(ref, g_ref_pos, sizeof ref);
    g_ts_shift = ts_shift_ms;
    auto ack = move_ack(m);
    std::memcpy(g_ref_pos, ref, sizeof ref);
    // Movement: overdue samples are dropped and our pauses hold them, as the
    // other player's (a menu or a loading screen replayed seconds of enemy
    // positions at once, analisis-ia2/P2 §5, P5 §6.4). Its actions are not.
    if (ack) enqueue_at(make_packet(off::kOpMoveActorAck, *ack), steady_ms() + delay_ms, /*movement=*/true);
}

void puppet_enemy_action(uint16_t req_op, const std::vector<uint8_t>& req, int64_t ts_shift_ms,
                         uint64_t delay_ms) {
    if (req.size() < 4) return;
    uint32_t key;
    std::memcpy(&key, req.data(), 4);
    float ref[3];
    std::memcpy(ref, g_ref_pos, sizeof ref);
    if (auto it = g_enemy_pos.find(key); it != g_enemy_pos.end()) {
        std::memcpy(g_ref_pos, it->second.data(), sizeof ref);
    }
    g_ts_shift = ts_shift_ms;
    std::optional<Bytes> ack;
    uint16_t op = 0;
    if (req_op == off::kOpStartActionReq) {
        ack = start_action_ack(req);
        op = off::kOpStartActionAck;
    } else if (req_op == off::kOpRotateActorReq) {
        ack = rotate_ack(req);
        op = off::kOpRotateActorAck;
    }
    std::memcpy(g_ref_pos, ref, sizeof ref);
    if (ack) enqueue_at(make_packet(op, *ack), steady_ms() + delay_ms, /*movement=*/false);
}

void puppet_remove_actor(uint32_t key) {
    g_enemy_pos.erase(key);
    Bytes payload = {0x04, 0x00, 0x00, 0x00, 0x00, 0x15, 0, 0, 0, 0};
    std::memcpy(payload.data() + 6, &key, 4);
    enqueue_at(make_packet(off::kOpRemoveActorAck, payload), steady_ms(), /*movement=*/false);
}

void puppet_inject_s2c(uint16_t op, const std::vector<uint8_t>& payload, uint64_t delay_ms) {
    enqueue_at(make_packet(op, payload), steady_ms() + delay_ms, /*movement=*/false);
}

void puppet_inject_s2c_batch(const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& packets) {
    const uint64_t due = steady_ms();
    AcquireSRWLockExclusive(&g_queue_lock);
    auto it = std::upper_bound(g_queue.begin(), g_queue.end(), due,
                               [](uint64_t d, const Pending& p) { return d < p.due_ms; });
    for (const auto& [op, payload] : packets) {
        it = g_queue.insert(it, {due, /*movement=*/false, make_packet(op, payload)});
        ++it;
    }
    ReleaseSRWLockExclusive(&g_queue_lock);
}

bool puppet_local_paused() { return g_paused_now.load(std::memory_order_relaxed); }

bool puppet_spawned() {
    std::lock_guard lock(g_mutex);
    return g_spawned;
}

std::atomic<uint8_t> g_pin_shape{4}, g_pin_colour{14};

void puppet_map_pin_style(uint8_t shape, uint8_t colour) {
    if (shape < off::kPinShapeCount) g_pin_shape = shape;
    if (colour < off::kPinColourCount) g_pin_colour = colour;
}

void puppet_map_pin(uint32_t owner, const std::optional<proto::Vec3>& pos, bool replace) {
    const uint64_t id = off::kCoopPinId;
    const uint64_t now = steady_ms();
    if (!pos || replace) {
        Bytes r(13, 0);
        std::memcpy(r.data(), &owner, 4);
        std::memcpy(r.data() + 4, &id, 8);
        r[12] = off::kPinGroupNormal;
        enqueue_at(make_packet(off::kOpRemovePinMarkerAck, r), now, /*movement=*/false);
    }
    if (!pos) return;
    // Same due time as the removal: the queue keeps their order.
    Bytes c(6 + 22, 0);
    c[0] = 0x04;
    std::memcpy(c.data() + 1, &owner, 4);
    c[5] = off::kPinGroupNormal;
    std::memcpy(c.data() + 6, &id, 8);
    const float xyz[3] = {pos->x, pos->y, pos->z};
    std::memcpy(c.data() + 14, xyz, sizeof xyz);
    c[26] = g_pin_shape.load();
    c[27] = g_pin_colour.load();
    enqueue_at(make_packet(off::kOpCreateOrChangePinMarkerAck, c), now, /*movement=*/false);
}

std::optional<proto::Vec3> puppet_position() {
    if (!puppet_spawned()) return std::nullopt;
    return game::actor_world_position(kPuppetPlayer);
}

// ---------------------------------------------------------------- horse
// While the sender rides, its own Moves carry the horse's world transform in
// Path.world (+0x94 position, +0xA0 rotation; caballo-coop.md §1.2) at ~36 Hz,
// whereas the horse itself reports only once a second. Derive a horse Move
// from every rider Move, with the same timestamp, so both copies interpolate
// in step and the rider stays in the saddle.
uint64_t g_horse_ridden_until = 0;

void ride_horse(const Bytes& rider_req, uint64_t now, uint64_t delay_ms) {
    if (!g_horse_spawned) return;
    Bytes h = rider_req;
    std::memcpy(h.data() + 0x00, &kPuppetHorse, 4);
    std::memcpy(h.data() + 0x04, rider_req.data() + 0x94, 12);  // horse position
    std::memcpy(h.data() + 0x10, rider_req.data() + 0xA0, 16);  // horse rotation
    std::memset(h.data() + 0x20, 0, 4);
    std::memcpy(h.data() + 0x4C, rider_req.data() + 0x94, 12);  // Path.local = world, no parent
    std::memcpy(h.data() + 0x58, rider_req.data() + 0xA0, 16);
    const float one[3] = {1.0f, 1.0f, 1.0f};
    std::memcpy(h.data() + 0x68, one, sizeof one);
    std::memset(h.data() + 0x8C, 0xFF, 8);
    std::memset(h.data() + 0x94, 0, 0x40);
    float hp[3];
    std::memcpy(hp, h.data() + 0x04, sizeof hp);
    if (!std::isfinite(hp[0]) || (hp[0] == 0.0f && hp[2] == 0.0f)) return;
    float ref[3];
    std::memcpy(ref, g_ref_pos, sizeof ref);  // keep the rider's reference position
    auto ack = move_ack(h);
    std::memcpy(g_ref_pos, ref, sizeof ref);
    if (!ack) return;
    enqueue_at(make_packet(off::kOpMoveActorAck, *ack), movement_due(now + delay_ms), /*movement=*/true);
    g_horse_ridden_until = now + 200;
}

// The horse's own messages (unridden moves, actions, turns).
void horse_event(proto::EventKind kind, const Bytes& payload, uint64_t now, uint64_t delay_ms) {
    if (!g_horse_spawned) return;
    Bytes load;
    uint64_t load_at = 0;
    {
        std::lock_guard lock(g_mutex);
        if (!g_horse_pending.empty()) {
            // Not loaded yet: only its first action loads it; the rest is moot.
            if (kind != proto::EventKind::StartAction) return;
            if (!game::find_actor(kPuppetPlayer)) return;  // the copy's player first
            load = std::move(g_horse_pending);
            g_horse_pending.clear();
            load_at = std::max(now, g_horse_not_before);
        }
    }
    if (!load.empty()) {
        float keep[3];
        std::memcpy(keep, g_ref_pos, sizeof keep);
        auto first = start_action_ack(payload);
        std::memcpy(g_ref_pos, keep, sizeof keep);
        if (!first) {
            std::lock_guard lock(g_mutex);
            g_horse_pending = std::move(load);  // wait for a usable one
            return;
        }
        // Same due time: the queue is stable, so the action lands right
        // behind the load (retried by the client until the horse exists).
        enqueue_at(std::move(load), load_at, /*movement=*/false);
        enqueue_at(make_packet(off::kOpStartActionAck, *first), load_at, /*movement=*/false);
        log::info("Puppet: horse load queued together with its first action ({} bytes)", payload.size());
        return;
    }
    uint16_t op = 0;
    std::optional<Bytes> ack;
    float ref[3];
    std::memcpy(ref, g_ref_pos, sizeof ref);  // the shift reference stays the rider's
    switch (kind) {
        case proto::EventKind::Move:
            // Ridden: its rare own Move repeats a timestamp we already used.
            if (now < g_horse_ridden_until) return;
            op = off::kOpMoveActorAck;
            ack = move_ack(payload);
            break;
        case proto::EventKind::StartAction:
            op = off::kOpStartActionAck;
            ack = start_action_ack(payload);
            break;
        case proto::EventKind::Rotate:
            op = off::kOpRotateActorAck;
            ack = rotate_ack(payload);
            break;
        default:
            return;
    }
    std::memcpy(g_ref_pos, ref, sizeof ref);
    if (ack) enqueue_at(make_packet(op, *ack), movement_due(now + delay_ms), /*movement=*/true);
}

void puppet_event(const proto::GameEvent& ev, int64_t ts_shift_ms, uint64_t delay_ms) {
    if (!puppet_spawned() || ev.payload.size() > proto::kMaxEventPayload) return;
    if (g_copy_hooked && steady_ms() - g_rope_seen_ms > kRopeWatchdogMs) {
        enqueue_at(rope_release(), steady_ms() + delay_ms, /*movement=*/true);
        g_copy_hooked = false;
        if (g_hooks_released++ < 50) log::info("Puppet: no rope action from the other player for 5 s: rope let go");
    }
    const uint32_t src = g_src_player_key;
    Bytes payload = ev.payload;
    // Moves arrive without their unused trailing history (local_capture emit).
    if (ev.kind == proto::EventKind::Move && payload.size() == 0xEA) payload.resize(514, 0);
    // The actor key leads every payload, after a fragment marker byte in
    // OtherEquipItemAck.
    const size_t key_at = ev.kind == proto::EventKind::Equip   ? 1
                          : ev.kind == proto::EventKind::Relay ? 2
                                                               : 0;
    if (payload.size() < key_at + 4) return;
    const uint32_t key = u32_at(payload, key_at);
    const uint32_t src_horse = g_src_horse_key;
    const bool is_horse = src_horse && key == src_horse;
    if (key != src && !is_horse) return;
    replace_u32(payload, src, kPuppetPlayer);
    if (src_horse) replace_u32(payload, src_horse, kPuppetHorse);
    if (const uint32_t user = g_src_user_key) {
        // The user key is often 0x90100000, whose bytes "00 00 10 90" also turn
        // up by chance inside other fields (a ReviveAck's "02 00 00 00 10 90 ..."
        // became 0xF0010002): in relayed server messages, only at 4-byte
        // aligned field offsets.
        if (ev.kind == proto::EventKind::Relay) {
            for (size_t i = 2; i + 4 <= payload.size(); i += 4) {
                if (u32_at(payload, i) == user) std::memcpy(payload.data() + i, &kPuppetUser, 4);
            }
        } else {
            replace_u32(payload, user, kPuppetUser);
        }
    }
    g_ts_shift = ts_shift_ms;
    const uint64_t now = steady_ms();
    // How long Moves wait before going in (see kHoldMs); actions: see action_hold_for.
    const uint64_t move_hold = move_hold_for(delay_ms);
    if (!g_action_hold) g_action_hold = delay_ms;

    if (is_horse) {
        horse_event(ev.kind, payload, now, move_hold);
        return;
    }

    uint16_t ack_op = 0;
    std::optional<Bytes> ack;
    switch (ev.kind) {
        case proto::EventKind::Move:
            if (payload.size() != 514) return;
            fill_gap_before(payload, move_hold);
            g_last_move = payload;
            g_last_move_ms = now;
            std::memcpy(&g_last_move_ts, payload.data() + 0xD5, 8);
            std::memcpy(g_last_move_pos, payload.data() + 0x04, sizeof g_last_move_pos);
            ack_op = off::kOpMoveActorAck;
            ack = move_ack(payload);
            if (u32_at(payload, 0x8C) == kPuppetHorse) ride_horse(payload, now, move_hold);
            {
                // Dismounted (parent back to none for a few Moves): unless our
                // copy really rides our copy of the horse, reload it. Only the
                // sender's horse counts: other parents (a pulled tree, a carried
                // animal, a boat) caused reload loops in live test 4.
                const uint32_t parent = u32_at(payload, 0x8C);
                const uint32_t horse = g_sender_horse;
                const bool riding = horse && (parent == kPuppetHorse || parent == horse);
                if (riding) {
                    g_sender_riding = true;
                    g_unmounted_moves = 0;
                } else if (g_sender_riding && ++g_unmounted_moves >= 5) {
                    g_sender_riding = false;
                    // Our horse copy may have been queued but not created by the
                    // game (live tests): look it up before trusting it.
                    bool keep = false;
                    {
                        std::lock_guard lock(g_mutex);
                        keep = g_horse_spawned && g_horse_pending.empty() && game::find_actor(kPuppetHorse);
                        g_keep_horse = keep;
                    }
                    g_reload_request = true;
                    log::info("Puppet: the sender dismounted; reloading the {} (no rider link)",
                              keep ? "rider, the horse stays" : "copy");
                }
            }
            break;

        case proto::EventKind::StartAction:
            // A StartAction right after a filled climb-up that still carries the
            // frozen start position: move it to where the sender ended up.
            if (payload.size() == 725 && g_recent_gap.valid) {
                if (now - g_recent_gap.detected_ms <= 300) {
                    for (size_t at : {size_t{0x8E}, size_t{0xD6}}) {
                        float v[3];
                        std::memcpy(v, payload.data() + at, sizeof v);
                        const float ex = v[0] - g_recent_gap.start[0], ey = v[1] - g_recent_gap.start[1],
                                    ez = v[2] - g_recent_gap.start[2];
                        if (ex * ex + ey * ey + ez * ez < 4.0f) {
                            std::memcpy(payload.data() + at, g_recent_gap.end, sizeof g_recent_gap.end);
                        }
                    }
                } else {
                    g_recent_gap.valid = false;
                }
            }
            ack_op = off::kOpStartActionAck;
            ack = start_action_ack(payload);
            break;

        case proto::EventKind::Rotate:
            // During a Move gap (climbing...), RotateActorReq keeps sending the
            // frozen grab position at +0x14; applied to the puppet it would drag
            // it back to the wall's base between synthesised moves. Use the
            // latest Move position instead (only when that field actually holds
            // a nearby position).
            if (payload.size() == 176 && now - g_last_move_ms >= kMoveSilenceMs &&
                std::isfinite(g_last_move_pos[0])) {
                float v[3];
                std::memcpy(v, payload.data() + 0x14, sizeof v);
                const float dx = v[0] - g_last_move_pos[0], dz = v[2] - g_last_move_pos[2];
                if (dx * dx + dz * dz < 30.0f * 30.0f) {
                    std::memcpy(payload.data() + 0x14, g_last_move_pos, sizeof g_last_move_pos);
                }
            }
            ack_op = off::kOpRotateActorAck;
            ack = rotate_ack(payload);
            break;

        case proto::EventKind::Equip:
            // OtherEquipItemAck: 04 | key | serial | n x {u32 item, u16 slot}.
            if (!g_equip_enabled || !EquipState::plausible(payload.data(), payload.size())) return;
            if (payload.size() == 15 && u32_at(payload, 9) == 0) {
                // A single removal: the MMO sent OtherUnequipItemAck for that.
                const uint16_t slot = static_cast<uint16_t>(payload[13] | (payload[14] << 8));
                enqueue_at(make_packet(off::kOpOtherUnequipItemAck,
                                       unequip_payload(kPuppetPlayer, slot, next_equip_serial(u32_at(payload, 5)))),
                           now + delay_ms, /*movement=*/false);
                g_equip_check_at = now + delay_ms + 1500;
                log::info("Puppet: equipment removed (slot {})", slot);
                return;
            }
            {
                const uint32_t serial = next_equip_serial(u32_at(payload, 5));
                std::memcpy(payload.data() + 5, &serial, 4);
            }
            // Not movement: a menu pause longer than the stale limit must not drop it.
            enqueue_at(make_packet(off::kOpOtherEquipItemAck, payload), now + delay_ms, /*movement=*/false);
            g_equip_check_at = now + delay_ms + 1500;
            log::info("Puppet: equipment change ({} slot(s))", (payload.size() - 9) / 6);
            return;

        case proto::EventKind::Relay: {
            // u16 opcode + a message the sender's server sent about the sender.
            uint16_t op = 0;
            std::memcpy(&op, payload.data(), 2);
            const off::RelayedAck* rule = nullptr;
            for (const auto& r : off::kRelayedAcks) {
                if (r.op == op) rule = &r;
            }
            if (!rule) return;
            Bytes body(payload.begin() + 2, payload.end());
            if (rule->size ? body.size() != rule->size : body.size() > 64) return;
            // Secondary key fields (who caused it) name actors of the sender's
            // world, unknown or different here: make them the copy itself.
            for (uint16_t at : rule->other) {
                if (!at || at + 4 > body.size()) continue;
                uint32_t v;
                std::memcpy(&v, body.data() + at, 4);
                if (is_actor_key(v) && !is_puppet_key(v)) std::memcpy(body.data() + at, &kPuppetPlayer, 4);
            }
            bool foreign = false;
            if (rule->fields_known) {
                // Only the key fields: the rest is data that may look like a key.
                foreign = !is_puppet_key(u32_at(body, 0));
                for (uint16_t at : rule->none) {
                    if (at && at + 4 <= body.size() && is_actor_key(u32_at(body, at))) foreign = true;
                }
            } else {
                foreign = has_foreign_key(body);
            }
            if (foreign) {
                if (g_relays_dropped++ % 50 == 0) {
                    log::info("Puppet: relayed opcode {} names another actor of the sender's world; "
                              "dropped ({} so far)",
                              op, g_relays_dropped);
                }
                return;
            }
            if (op == off::kOpRemoteCatchResultAck) {
                if (!hook_tree_here(body)) return;
                // Still on the last tree (its release was missed): let go first.
                if (g_copy_hooked) enqueue_at(rope_release(), now + delay_ms, /*movement=*/true);
                g_copy_hooked = true;
                g_hook_slot = body[4];
                g_rope_seen_ms = now;
                if (g_rope_probes < 20) g_rope_probe_at = now + delay_ms + 1500;
            }
            enqueue_at(make_packet(op, body), now + delay_ms, /*movement=*/true);
            return;
        }

        case proto::EventKind::Docking:
            // Resolved against the action the puppet is playing, which was
            // queued before with the same delay.
            if (payload.size() != 16) return;
            enqueue_at(make_packet(off::kOpChangeDockingSocketByFrameEventAck, payload),
                       action_due(now + g_action_hold), /*movement=*/true);
            return;

        case proto::EventKind::WallUp: {
            // Must arrive just before the climb-up StartAction it belongs to.
            constexpr uint64_t kLead = 40;
            if (auto w = wallup_ack(payload)) {
                enqueue_at(make_packet(off::kOpUpdateWallUpDataAck, *w),
                           action_due(now + (g_action_hold > kLead ? g_action_hold - kLead : 0)), /*movement=*/true);
                g_events.fetch_add(1);
            }
            return;
        }
    }
    if (!ack) {
        log::warn("Puppet: event kind {} has unexpected size {}; dropped",
                  static_cast<int>(ev.kind), ev.payload.size());
        return;
    }
    if (ev.kind == proto::EventKind::Move) {
        enqueue_at(make_packet(ack_op, *ack), movement_due(now + move_hold), /*movement=*/true);
    } else {
        g_action_hold = action_hold_for(*ack, ev.kind == proto::EventKind::StartAction ? 0x160 : 0x9C, delay_ms);
        const uint64_t due = action_due(now + g_action_hold);
        if (ev.kind == proto::EventKind::StartAction && !is_horse && g_copy_hooked && payload.size() >= 12) {
            const uint32_t next = u32_at(payload, 8);
            if (next) {
                g_copy_pulling = std::find(std::begin(kRopePullActions), std::end(kRopePullActions), next) !=
                                 std::end(kRopePullActions);
            }
            if (next && is_rope_action(next)) {
                g_rope_seen_ms = now;
            } else if (next) {
                // The sender is off the rope: the copy too, with that action.
                enqueue_at(rope_release(), due, /*movement=*/true);
                g_copy_hooked = false;
                if (g_hooks_released++ < 50) {
                    log::info("Puppet: the other player let go of the rope (action 0x{:08X})", next);
                }
            }
        }
        enqueue_at(make_packet(ack_op, *ack), due, /*movement=*/true);
    }
    if (g_events.fetch_add(1) % 500 == 0) {
        log::info("Puppet: {} events replayed (ts shift {} ms, delay {} ms)", g_events.load(),
                  ts_shift_ms, delay_ms);
    }
}

uint64_t g_ride_forced_logged = 0;

// A copy that stays far from where its moves put it, while they keep coming,
// is stuck: load it again. Not while riding (the horse moves it) and at most
// every 20 s.
void watch_stuck() {
    static uint64_t off_since = 0, last_reload = 0;
    const uint64_t now = steady_ms();
    float told[3];
    uint64_t told_ms;
    AcquireSRWLockExclusive(&g_queue_lock);
    std::memcpy(told, g_told_pos, sizeof told);
    told_ms = g_told_ms;
    ReleaseSRWLockExclusive(&g_queue_lock);
    const auto at = puppet_spawned() ? game::actor_world_position(kPuppetPlayer) : std::nullopt;
    bool off_path = false;
    if (at && told_ms && now - told_ms < 1000 && now >= g_horse_ridden_until + 2000) {
        const float dx = at->x - told[0], dz = at->z - told[2];
        off_path = dx * dx + dz * dz > 6.0f * 6.0f;
    }
    if (!off_path) {
        off_since = 0;
        return;
    }
    if (!off_since) off_since = now;
    if (now - off_since < 4000 || now - last_reload < 20000) return;
    log::warn("Puppet: the copy is stuck ({:.1f} m from where its moves put it for 4 s); reloading it",
              std::sqrt((at->x - told[0]) * (at->x - told[0]) + (at->z - told[2]) * (at->z - told[2])));
    g_reload_request = true;
    last_reload = now;
    off_since = 0;
}

// What the copy's equipment component holds after an injection, to tell
// whether the client applied it: ClientEquipSlotActorComponent
// [[actor+0x68]+0x38], serial at +0x18, slot list [+0x90] (data +8, count
// +0x10, 0xD0-byte entries with the u16 slot at +0xC8; ropa-al-desmontar.md
// section 5.4). Read only, every read guarded.
void log_copy_gear() {
    const auto actor = game::find_actor(kPuppetPlayer);
    if (!actor) return;
    const auto comps = game::read_ptr(*actor + off::kActor_ComponentTable);
    const auto comp = comps ? game::read_ptr(*comps + 0x38) : std::nullopt;
    if (!comp) {
        log::info("Puppet: copy gear: equipment component not readable");
        return;
    }
    const auto serial = game::read<uint32_t>(*comp + 0x18);
    const auto list = game::read_ptr(*comp + 0x90);
    const auto data = list ? game::read_ptr(*list + 8) : std::nullopt;
    const auto count = list ? game::read<uint32_t>(*list + 0x10) : std::nullopt;
    std::string slots;
    if (data && count && *count <= 64) {
        for (uint32_t i = 0; i < *count; ++i) {
            if (const auto slot = game::read<uint16_t>(*data + i * 0xD0 + 0xC8)) slots += std::format(" {}", *slot);
        }
    }
    log::info("Puppet: copy gear now: serial {} | {} slot(s):{}", serial ? static_cast<int64_t>(*serial) : -1,
              count ? static_cast<int64_t>(*count) : -1, slots.empty() ? std::string(" ?") : slots);
}

namespace {

// Slot 0 of an actor's rope component, read-only (arbol-cuerda-2.md fix 3).
std::string rope_slot_text(uint32_t key) {
    if (!key) return "no key";
    const auto actor = game::find_actor(key);
    if (!actor) return "no actor";
    const auto c1 = game::read_ptr(*actor + 0x68);
    const auto comp = c1 ? game::read_ptr(*c1 + 0x78) : std::nullopt;
    if (!comp) return "no rope component";
    const auto u8 = [&](size_t off) { return game::read<uint8_t>(*comp + off).value_or(0xEE); };
    const auto u64 = [&](size_t off) { return game::read<uint64_t>(*comp + off).value_or(0xEEEEEEEE); };
    const auto f32 = [&](size_t off) { return game::read<float>(*comp + off).value_or(-1.0f); };
    // The current action's node (+0xF6 == 1 while pulling, hypothesis).
    uint8_t f6 = 0xEE;
    if (const auto a = game::read_ptr(*actor + 0x68)) {
        if (const auto b = game::read_ptr(*a + 0x40)) {
            if (const auto c = game::read_ptr(*b + 0x88)) {
                if (const auto node = game::read_ptr(*c + 0x10)) f6 = game::read<uint8_t>(*node + 0xF6).value_or(0xEE);
            }
        }
    }
    // The rope mesh (handle +0x9A0 -> [h+8]) and its two end nodes (arbol-cuerda-4 R0).
    const auto alive = [&](size_t off) {
        const auto h = game::read_ptr(*comp + off);
        return h && game::read_ptr(*h + 8) ? "ok" : "no";
    };
    uint8_t mesh15 = 0xEE, mesh30 = 0xEE;
    if (const auto h = game::read_ptr(*comp + 0x9A0)) {
        if (const auto o = game::read_ptr(*h + 8)) {
            mesh15 = game::read<uint8_t>(*o + 0x15).value_or(0xEE);
            mesh30 = game::read<uint8_t>(*o + 0x30).value_or(0xEE);
        }
    }
    return std::format("type {} piece 0x{:X} phase {} ({}/{}) flight {:.2f}/{:.2f} rope {:X} {:X} | fx +AC0 {} +ABC {:.2f} "
                       "+AB8 {:.2f} | spline +A98 {} trail n {} d {:.2f} | +CA8 {} +D00 0x{:X} | action +F6 {} | bend "
                       "+4F4 {} f {:.2f} v {:.2f} | mesh {} +15 {} +30 {} A {} B {}",
                       u8(0x20), u64(0x40), u8(0x9BC), u8(0x9F9), u8(0x9FA), f32(0xA90), f32(0xA94), u64(0x928),
                       u64(0x930), u8(0xAC0), f32(0xABC), f32(0xAB8), u8(0xA98),
                       game::read<uint32_t>(*comp + 0xAB0).value_or(0xEEEE), f32(0xAA0), u8(0xCA8), u64(0xD00), f6,
                       u8(0x4F4), f32(0x4E0), f32(0x4E4), alive(0x9A0), mesh15, mesh30, alive(0x950), alive(0x958));
}

}  // namespace

void puppet_tick() {
    horse_diag_tick();
    {
        static uint64_t timing_at = 0;
        if (steady_ms() - timing_at >= 10000) {
            timing_at = steady_ms();
            Timing t[2];
            AcquireSRWLockExclusive(&g_queue_lock);
            std::swap(t[0], g_timing[0]);
            std::swap(t[1], g_timing[1]);
            ReleaseSRWLockExclusive(&g_queue_lock);
            const auto pct = [](std::vector<int64_t>& v, double q) {
                if (v.empty()) return int64_t{0};
                const size_t i = std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())));
                std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(i), v.end());
                return v[i];
            };
            for (int c = 0; c < 2; ++c) {
                if (t[c].wait.empty()) continue;
                const size_t n = t[c].margin.size();
                const auto late = std::count_if(t[c].margin.begin(), t[c].margin.end(), [](int64_t m) { return m < 0; });
                log::info("Timing: {} movement, 10 s: {} samples | queue wait p50 {} p95 {} ms | margin to drawing "
                          "p5 {} p50 {} ms, late {} ({:.0f}%)",
                          c ? "enemy copies'" : "the other player's", n, pct(t[c].wait, 0.5), pct(t[c].wait, 0.95),
                          pct(t[c].margin, 0.05), pct(t[c].margin, 0.5), late, 100.0 * late / n);
            }
        }
    }
    if (const uint64_t at = g_rope_probe_at.load(); at && steady_ms() >= at) {
        g_rope_probe_at = g_copy_hooked && g_rope_probes < 40 ? steady_ms() + 3000 : 0;
        ++g_rope_probes;
        log::info("Puppet: rope probe | copy{}: {} | ours: {} | bends {} | rope end: |B-target| copy {:.2f} m "
                  "ours {:.2f} m, set {} times",
                  g_copy_pulling ? " (pulling)" : "", rope_slot_text(kPuppetPlayer),
                  rope_slot_text(capture_own_player_key()), g_bends.load(), g_end_gap_copy.load(),
                  g_end_gap_ours.load(), g_rope_end_sets.load());
    }
    {
        // Our own hook (phase 2-3): probe too, to compare with the copy's.
        static uint64_t checked = 0;
        static bool own_was = false;
        if (steady_ms() - checked >= 1000) {
            checked = steady_ms();
            bool own_on = false;
            if (const auto actor = game::find_actor(capture_own_player_key())) {
                if (const auto c1 = game::read_ptr(*actor + 0x68)) {
                    if (const auto comp = game::read_ptr(*c1 + 0x78)) {
                        const uint8_t type = game::read<uint8_t>(*comp + 0x20).value_or(0xEE);
                        const uint8_t phase = game::read<uint8_t>(*comp + 0x9BC).value_or(0);
                        own_on = type == 0 && (phase == 2 || phase == 3);
                    }
                }
            }
            if (own_on && !own_was && g_rope_probes < 40 && !g_rope_probe_at) g_rope_probe_at = steady_ms() + 800;
            own_was = own_on;
        }
    }
    if (g_rope_bend || g_rope_effect || g_rope_end) {
        static uint64_t refreshed = 0;
        if (steady_ms() - refreshed >= 250) {
            refreshed = steady_ms();
            g_copy_actor = game::find_actor(kPuppetPlayer).value_or(0);
            const uint32_t own = capture_own_player_key();
            g_own_actor = own ? game::find_actor(own).value_or(0) : 0;
        }
    }
    watch_stuck();
    if (const bool paused = g_paused_now.load(std::memory_order_relaxed); paused != g_paused_logged) {
        log::info("Puppet: this game is {}", paused ? "paused (menu): copy movement held back"
                                                     : "running again: copy movement resumes");
        g_paused_logged = paused;
    }
    if (const uint64_t n = g_restamped.load(std::memory_order_relaxed); n != g_restamped_logged) {
        static uint64_t last_log = 0;
        if (steady_ms() - last_log >= 5000) {
            log::info("Puppet: {} movement timestamps kept within the client's clock so far ({} repeated "
                      "positions dropped)",
                      n, g_dropped_repeats.load(std::memory_order_relaxed));
            last_log = steady_ms();
            g_restamped_logged = n;
        }
    }
    if (const uint64_t at = g_equip_check_at.load(); at && steady_ms() >= at) {
        g_equip_check_at = 0;
        log_copy_gear();
    }
    const uint64_t skipped = g_layers_skipped.load(std::memory_order_relaxed);
    if (skipped != g_layers_skipped_logged) {
        static uint64_t last_log = 0;
        const uint64_t now = steady_ms();
        if (g_layers_skipped_logged == 0 || now - last_log >= 5000) {
            log::info("Puppet: copy 0x{:08X} has no action yet; its animation update was skipped {} times",
                      g_layers_skipped_key.load(std::memory_order_relaxed), skipped);
            last_log = now;
            g_layers_skipped_logged = skipped;
        }
    }
    const uint64_t guard = g_layer_guard_fired.load(std::memory_order_relaxed);
    if (guard != g_layer_guard_logged && g_layer_guard_logged < 1000000) {
        static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        static uint64_t last_log = 0;
        const uint64_t now = steady_ms();
        if (g_layer_guard_logged == 0 || now - last_log >= 5000) {
            const uintptr_t caller = g_layer_guard_caller.load(std::memory_order_relaxed);
            log::info("Puppet: crash guard: an action layer had no action ({} times, from exe+0x{:X})",
                      guard, caller >= base ? caller - base : caller);
            last_log = now;
            g_layer_guard_logged = guard;
        }
    }
    const uint64_t ride = g_ride_forced.load(std::memory_order_relaxed);
    if (ride != g_ride_forced_logged && g_ride_forced_logged < 50) {
        log::info("Puppet: mount/dismount enabled ({} times)", ride);
        g_ride_forced_logged = ride;
    }
    const uint64_t forced = g_climb_forced.load(std::memory_order_relaxed);
    if (forced != g_climb_forced_logged && g_climb_forced_logged < 50) {
        log::info("Puppet: wall climb enabled ({} times)", forced);
        g_climb_forced_logged = forced;
    }
    const uint64_t other = g_climb_other_fails.load(std::memory_order_relaxed);
    if (other != g_climb_other_logged && g_climb_other_logged < 20) {
        log::info("Puppet: PointClimb Enter failed for another actor (owner 0x{:08X}) [{}]",
                  g_climb_last_owner.load(std::memory_order_relaxed), other);
        g_climb_other_logged = other;
    }
}

// The timestamp of an injected movement Ack, if it is one: its offset and buffer.
bool movement_ts(const Bytes& pk, size_t& at, InjectClock::Buffer& buffer) {
    if (pk.size() < 5 + 4) return false;
    const uint16_t op = static_cast<uint16_t>(pk[0] | (pk[1] << 8));
    if (op == off::kOpMoveActorAck) at = 5 + 0xD5, buffer = InjectClock::Buffer::Position;
    else if (op == off::kOpStartActionAck) at = 5 + 0x160, buffer = InjectClock::Buffer::Both;
    else if (op == off::kOpRotateActorAck) at = 5 + 0x9C, buffer = InjectClock::Buffer::Rotation;
    else return false;
    return pk.size() >= at + 8;
}

namespace {

// The piece id our client gives the tree with this scene id, as the game finds
// it at 0x5947C7: mgr = [[[exe+0x6D69190]+0xC8]]+0x80; 0x1851010(mgr, &att,
// &scene) attaches the scene object (att+8); its body [[obj+0x58]+0xE0] (when
// [obj+0x58]+0x15 != 1) gives the piece through vf 0x70; att is released with
// 0x1434880. ~0 if anything is missing. Client network thread only, where the
// game handles RemoteCatchResultAck itself.
uintptr_t scene_object_manager(uintptr_t base) {
    const auto world = game::read_ptr(base + 0x6D69190);
    const auto a = world ? game::read_ptr(*world + 0xC8) : std::nullopt;
    const auto b = a ? game::read_ptr(*a) : std::nullopt;
    const auto m = b ? game::read_ptr(*b + 0x80) : std::nullopt;
    return m ? *m : 0;
}

uint64_t rope_piece_of(uintptr_t base, const uint8_t* scene) {
    uint64_t piece = ~0ull;
    alignas(16) uint8_t att[0x40] = {};
    const uintptr_t mgr = scene_object_manager(base);
    if (!mgr) return piece;
    using Find = void*(__fastcall*)(uintptr_t, void*, const void*);
    using Release = void(__fastcall*)(void*);
    using PieceOf = uint64_t(__fastcall*)(uintptr_t);
    bool attached = false;
    g_rope_lookup_faulted = true;  // stays so if the game code below faults
    __try {
        reinterpret_cast<Find>(base + 0x1851010)(mgr, att, scene);
        attached = true;
        uintptr_t obj = 0;
        std::memcpy(&obj, att + 8, sizeof obj);
        const uintptr_t h = obj ? *reinterpret_cast<const uintptr_t*>(obj + 0x58) : 0;
        if (h && *reinterpret_cast<const uint8_t*>(h + 0x15) != 1) {
            const uintptr_t body = *reinterpret_cast<const uintptr_t*>(h + 0xE0);
            if (body) {
                const auto vt = *reinterpret_cast<const uintptr_t* const*>(body);
                piece = reinterpret_cast<PieceOf>(vt[0x70 / 8])(body);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        piece = ~0ull;
        attached = false;
        return piece;
    }
    if (attached) {
        __try {
            const uintptr_t vt = base + 0x5725E48;  // ScopeAttacherBase, as the game leaves it
            std::memcpy(att, &vt, sizeof vt);
            reinterpret_cast<Release>(base + 0x1434880)(att);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return ~0ull;
        }
    }
    g_rope_lookup_faulted = false;
    return piece;
}

// A RemoteCatchResultAck of the copy that names no piece: ours of that tree.
void fill_rope_piece(std::vector<uint8_t>& out) {
    if (!g_rope_lookup || out.size() != 5 + 97 || out[0] != (off::kOpRemoteCatchResultAck & 0xFF) ||
        out[1] != (off::kOpRemoteCatchResultAck >> 8)) {
        return;
    }
    uint32_t key;
    uint64_t piece;
    std::memcpy(&key, out.data() + 5, 4);
    std::memcpy(&piece, out.data() + 5 + kRemoteCatchPiece, 8);
    if (key != kPuppetPlayer || piece != ~0ull) return;
    // No scene object (a point in world coordinates): the game does not look.
    static const uint8_t kNone[12] = {};
    if (std::memcmp(out.data() + 5 + off::kRemoteCatchSceneId, kNone, sizeof kNone) == 0) return;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    piece = rope_piece_of(base, out.data() + 5 + off::kRemoteCatchSceneId);
    ++g_rope_lookups;
    if (g_rope_lookup_faulted) {
        // Never again this session: the game's lookup faulted here once.
        g_rope_lookup = false;
        log::error("Puppet: the tree piece lookup faulted; it stays off for this session");
        return;
    }
    if (piece == ~0ull || piece == 0) {
        if (g_rope_lookups - g_rope_lookup_found <= 20) log::info("Puppet: the tree's piece was not found here; the rope goes to it at rest");
        return;
    }
    std::memcpy(out.data() + 5 + kRemoteCatchPiece, &piece, 8);
    if (g_rope_lookup_found++ < 20) log::info("Puppet: the tree's piece here is 0x{:016X}", piece);
}

}  // namespace

namespace {

// SetBend on the tree the copy holds, as 0x1190FD0 does for the local player:
// node = 0x5B1600(comp, 0); tree = 0x491AC0(node); hook point from
// 0x5A4760(comp, 0, out) (world x/z at out+0x1C/+0x24); 0x349EA10(tree, dir,
// strength), dir towards the copy, y 0. False if it faulted (then never again).
bool bend_tree(uintptr_t base, uintptr_t copy, uintptr_t own, float copy_x, float copy_z, float strength) {
    using Node = uintptr_t(__fastcall*)(uintptr_t, int);
    using Tree = uintptr_t(__fastcall*)(uintptr_t);
    using Point = bool(__fastcall*)(uintptr_t, int, void*);
    using Bend = void(__fastcall*)(uintptr_t, const float*, float);
    __try {
        const uintptr_t comp =
            *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(copy + 0x68) + 0x78);
        if (!comp || *reinterpret_cast<const uint8_t*>(comp + 0x20) != 0 ||
            *reinterpret_cast<const uint8_t*>(comp + 0x9BC) != 3) {
            return true;
        }
        // Not while our own player is on a tree too (both threads on one tree).
        if (own) {
            const uintptr_t own_comp =
                *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(own + 0x68) + 0x78);
            if (own_comp && *reinterpret_cast<const uint8_t*>(own_comp + 0x20) == 0 &&
                *reinterpret_cast<const uint8_t*>(own_comp + 0x9BC) == 3) {
                return true;
            }
        }
        const uintptr_t node = reinterpret_cast<Node>(base + 0x5B1600)(comp, 0);
        if (!node) return true;
        // Only a tree the game itself would bend (0x1191078): p = [node+0x108]
        // ->vf(0x328)(), with p and byte p+0x18 set.
        using CanBend = uintptr_t(__fastcall*)(uintptr_t);
        const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(node + 0x108);
        if (!obj) return true;
        const auto vt = *reinterpret_cast<const uintptr_t* const*>(obj);
        const uintptr_t p = reinterpret_cast<CanBend>(vt[0x328 / 8])(obj);
        if (!p || *reinterpret_cast<const uint8_t*>(p + 0x18) == 0) return true;
        const uintptr_t tree = reinterpret_cast<Tree>(base + 0x491AC0)(node);
        if (!tree) return true;
        alignas(16) uint8_t out[0x40] = {};
        if (!reinterpret_cast<Point>(base + 0x5A4760)(comp, 0, out)) return true;
        float hx, hz;
        std::memcpy(&hx, out + 0x1C, 4);
        std::memcpy(&hz, out + 0x24, 4);
        float dir[3] = {copy_x - hx, 0.0f, copy_z - hz};
        const float len = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
        if (!(len > 0.01f) || !std::isfinite(len)) return true;
        dir[0] /= len, dir[2] /= len;
        reinterpret_cast<Bend>(base + 0x349EA10)(tree, dir, strength);
        // The rope's flex, as the local pull leaves it (arbol-cuerda-4 R2): visual only.
        *reinterpret_cast<float*>(comp + 0x4E0) = strength;
        *reinterpret_cast<float*>(comp + 0x4E4) = kBendPerSecond;
        *reinterpret_cast<float*>(comp + 0x4E8) = dir[0];
        *reinterpret_cast<float*>(comp + 0x4EC) = 0.0f;
        *reinterpret_cast<float*>(comp + 0x4F0) = dir[2];
        *reinterpret_cast<uint8_t*>(comp + 0x4F4) = 1;
        g_flex_actor.store(copy, std::memory_order_relaxed);
        g_flex_comp.store(comp, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The flex off again (the copy stopped pulling or let go). Only on the same
// copy, through its live component: a copy removed meanwhile left a freed one.
bool clear_flex(uintptr_t copy) {
    const uintptr_t comp = g_flex_comp.exchange(0, std::memory_order_relaxed);
    const uintptr_t actor = g_flex_actor.exchange(0, std::memory_order_relaxed);
    if (!comp || actor != copy) return true;
    __try {
        if (*reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(copy + 0x68) + 0x78) != comp) {
            return true;
        }
        *reinterpret_cast<uint8_t*>(comp + 0x4F4) = 0;
        *reinterpret_cast<float*>(comp + 0x4E0) = 0.0f;
        *reinterpret_cast<float*>(comp + 0x4E4) = 0.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// R1: the rope's effect state on both slots of the copy, on while its rope is
// on a tree (type 0, phase 2-3), off otherwise; written only when it changes,
// as 0x595FE0 does (no call: that one takes the actor's lock, we are inside
// its update). False if it faulted.
bool rope_effect(uintptr_t copy, bool hooked) {
    __try {
        const uintptr_t comp =
            *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(copy + 0x68) + 0x78);
        if (!comp) return true;
        const uint8_t phase = *reinterpret_cast<const uint8_t*>(comp + 0x9BC);
        // Flight only: the local player has it off once on the tree (phase 3,
        // arbol-cuerda-5.md §4.5).
        const bool on = hooked && *reinterpret_cast<const uint8_t*>(comp + 0x20) == 0 && phase == 2;
        const bool is_on = g_effect_actor == copy;
        if (on == is_on) return true;
        for (uintptr_t r = 0; r < 2; ++r) {
            *reinterpret_cast<uint8_t*>(comp + 0xAC0 + r * 0x1A0) = on ? 1 : 0;
            *reinterpret_cast<float*>(comp + 0xABC + r * 0x1A0) = on ? 1.0f : 0.0f;
        }
        g_effect_actor = on ? copy : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A node's world position: 0x26D3940(node, &xf) fills an xf (+0x1C position,
// +0x28/+0x2A int16 sectors of 1000 m on x/z).
bool node_world(uintptr_t base, uintptr_t node, float out[3]) {
    using Get = void*(__fastcall*)(uintptr_t, void*);
    alignas(16) uint8_t xf[0x30] = {};
    reinterpret_cast<Get>(base + 0x26D3940)(node, xf);
    float p[3];
    int16_t sx, sz;
    std::memcpy(p, xf + 0x1C, sizeof p);
    std::memcpy(&sx, xf + 0x28, 2);
    std::memcpy(&sz, xf + 0x2A, 2);
    out[0] = p[0] + 1000.0f * sx, out[1] = p[1], out[2] = p[2] + 1000.0f * sz;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// R5, on the per-slot tick (rdi = comp, sil = slot). False if it faulted.
bool rope_end(uintptr_t comp, uint8_t slot) {
    using Target = bool(__fastcall*)(uintptr_t, int, void*);
    using Split = void(__fastcall*)(void*);
    using SetWorld = void(__fastcall*)(uintptr_t, const void*, int, int);
    if (slot != 0 || !comp) return true;  // a tree: slot 0
    const uintptr_t copy = g_copy_actor.load(std::memory_order_relaxed);
    const uintptr_t own = g_own_actor.load(std::memory_order_relaxed);
    __try {
        const uintptr_t actor = *reinterpret_cast<const uintptr_t*>(comp + 8);
        const bool is_copy = actor && actor == copy, is_own = actor && actor == own;
        if (!is_copy && !is_own) return true;
        if (*reinterpret_cast<const uint8_t*>(comp + 0x20) != 0 || *reinterpret_cast<const uint8_t*>(comp + 0x9BC) != 3) {
            return true;
        }
        if (is_copy && !g_copy_hooked.load(std::memory_order_relaxed)) return true;
        const uintptr_t h = *reinterpret_cast<const uintptr_t*>(comp + 0x958);
        const uintptr_t o = h ? *reinterpret_cast<const uintptr_t*>(h + 8) : 0;
        if (!o || *reinterpret_cast<const uint8_t*>(o + 0x15) != 0) return true;  // node B alive
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        alignas(16) uint8_t xf[0x30] = {};
        const float unit[7] = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(xf, unit, sizeof unit);
        if (!reinterpret_cast<Target>(base + 0x5A40E0)(comp, 0, xf)) return true;
        float t[3], b[3];
        std::memcpy(t, xf + 0x1C, sizeof t);
        if (node_world(base, o - 0x28, b)) {
            const float gap = std::sqrt((b[0] - t[0]) * (b[0] - t[0]) + (b[1] - t[1]) * (b[1] - t[1]) +
                                        (b[2] - t[2]) * (b[2] - t[2]));
            (is_copy ? g_end_gap_copy : g_end_gap_ours).store(gap, std::memory_order_relaxed);
        }
        if (!is_copy) return true;  // ours: measured only
        std::memset(xf + 0x28, 0, 4);
        reinterpret_cast<Split>(base + 0x50EA00)(xf);
        reinterpret_cast<SetWorld>(base + 0x26D4B40)(o - 0x28, xf, 0, 1);
        g_rope_end_sets.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void on_rope_slot_tick(safetyhook::Context& ctx) {
    if (!g_rope_end.load(std::memory_order_relaxed)) return;
    if (!rope_end(ctx.rdi, static_cast<uint8_t>(ctx.rsi & 0xFF))) {
        g_rope_end = false;
        log::error("Puppet: placing the copy's rope end faulted; it stays off for this session");
    }
}

// [control+8]: the actor whose control this is (0 if unreadable).
uintptr_t control_actor(uintptr_t control) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(control + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Every actor's control update, on the game's actor-update threads: cheap
// unless it is the copy.
void on_actor_update(safetyhook::Context& ctx) {
    const bool bend = g_rope_bend.load(std::memory_order_relaxed);
    const bool effect = g_rope_effect.load(std::memory_order_relaxed);
    if (!bend && !effect) return;
    const uintptr_t copy = g_copy_actor.load(std::memory_order_relaxed);
    if (!copy || !ctx.rsi || control_actor(ctx.rsi) != copy) return;
    const bool hooked = g_copy_hooked.load(std::memory_order_relaxed);
    if (g_effect_actor && g_effect_actor != copy) g_effect_actor = 0;  // a new copy starts with it off
    if (effect && !rope_effect(copy, hooked)) {
        g_rope_effect = false;
        log::error("Puppet: the copy's rope effect state faulted; it stays off for this session");
    }
    if (!bend) return;
    if (!hooked || !g_copy_pulling.load(std::memory_order_relaxed)) {
        g_pull_seconds.store(0.0f, std::memory_order_relaxed);
        if (!clear_flex(copy)) {
            g_rope_bend = false;
            log::error("Puppet: clearing the copy's rope flex faulted; the tree bend stays off for this session");
        }
        return;
    }
    const float dt = ctx.xmm6.f32[0];
    float t = g_pull_seconds.load(std::memory_order_relaxed);
    if (dt > 0.0f && dt < 0.5f) t += dt;
    g_pull_seconds.store(t, std::memory_order_relaxed);
    const float strength = std::min(kBendMax, kBendPerSecond * t);
    // Where its moves put the copy (no actor-table walk on this hot path).
    const float x = g_told_x.load(std::memory_order_relaxed), z = g_told_z.load(std::memory_order_relaxed);
    if (!std::isfinite(x) || !std::isfinite(z)) return;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!bend_tree(base, copy, g_own_actor.load(std::memory_order_relaxed), x, z, strength)) {
        g_rope_bend = false;
        log::error("Puppet: bending the tree for the copy faulted; it stays off for this session");
        return;
    }
    g_bends.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

std::atomic<bool> g_rope_bend_wanted{false};
void puppet_want_rope_bend(bool on) { g_rope_bend_wanted = on; }

bool puppet_install_rope_bend() {
    if (!g_rope_bend_wanted) return false;
    if (g_bend_hook) return true;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const uint8_t kHead[] = {0x48, 0x8B, 0x8E, 0xC8, 0x00, 0x00, 0x00, 0xE8};  // mov rcx,[rsi+0xC8]; call
    uint8_t head[sizeof kHead];
    if (base != off::kImageBase || !game::safe_read(base + 0x86A576, head, sizeof head) ||
        std::memcmp(head, kHead, sizeof head) != 0) {
        log::warn("Puppet: actor update not recognised; the tree will not bend for the other player");
        return false;
    }
    g_bend_hook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x86A576), on_actor_update);
    g_rope_bend = static_cast<bool>(g_bend_hook);
    g_rope_effect = g_rope_bend.load();
    log::info("Puppet: tree bend and rope effect for the other player's rope {}",
              g_rope_bend ? "installed" : "FAILED to install");
    // R5 (arbol-cuerda-5.md): movzx r8d,sil; vmovaps xmm1,xmm6; mov rcx,rdi; call 0x5AEAA0.
    static const uint8_t kSlotTick[] = {0x44, 0x0F, 0xB6, 0xC6, 0xC5, 0xF8, 0x28, 0xCE, 0x48, 0x8B, 0xCF, 0xE8};
    uint8_t at[sizeof kSlotTick];
    if (g_rope_bend && !g_rope_end_hook && game::safe_read(base + 0x59D89A, at, sizeof at) &&
        std::memcmp(at, kSlotTick, sizeof at) == 0) {
        g_rope_end_hook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x59D89A), on_rope_slot_tick);
        g_rope_end = static_cast<bool>(g_rope_end_hook);
    }
    log::info("Puppet: the other player's rope end on the tree: {}", g_rope_end ? "installed" : "NOT installed");
    return g_rope_bend;
}

void puppet_set_rope_lookup(bool on) { g_rope_lookup = on; }

void puppet_drop_stale_movement() {
    if (!TryAcquireSRWLockExclusive(&g_queue_lock)) return;
    const uint64_t now = steady_ms();
    const size_t before = g_queue.size();
    std::erase_if(g_queue, [&](const Pending& p) { return p.movement && p.due_ms + kStaleMovementMs < now; });
    g_stale_dropped += before - g_queue.size();
    ReleaseSRWLockExclusive(&g_queue_lock);
}


bool puppet_take_injection(std::vector<uint8_t>& out) {
    if (!TryAcquireSRWLockExclusive(&g_queue_lock)) return false;
    // Movement that is long overdue (the game sat in a menu and did not
    // receive packets) would replay seconds of the past at once (capture
    // 183530: 722 packets, 14 s): dropped. Loads and states still go in.
    const uint64_t now = steady_ms();
    while (!g_queue.empty() && g_queue.front().movement && g_queue.front().due_ms + kStaleMovementMs < now) {
        g_queue.pop_front();
        ++g_stale_dropped;
    }
    // This runs on the client's network thread: the game clock read here is
    // the one its handlers compare our timestamps with.
    const auto game_now = capture_game_clock_trusted() ? game::client_game_clock_ms() : std::nullopt;
    const bool paused = game_now && g_local_pause.update(*game_now, now);
    g_paused_now.store(paused, std::memory_order_relaxed);
    // Paused: only what is not movement (loads, removals, gear) goes in.
    bool has = false;
    for (;;) {
        auto it = g_queue.begin();
        while (paused && it != g_queue.end() && it->due_ms <= now && it->movement) ++it;
        if (it == g_queue.end() || it->due_ms > now) break;
        const uint64_t due = it->due_ms;
        out = std::move(it->packet);
        g_queue.erase(it);
        size_t ts_at = 0;
        InjectClock::Buffer buffer{};
        if (game_now && movement_ts(out, ts_at, buffer)) {
            uint32_t key;
            uint64_t ts;
            std::memcpy(&key, out.data() + 5, 4);
            std::memcpy(&ts, out.data() + ts_at, 8);
            const uint64_t fixed = g_inject_clock.stamp(key, buffer, ts, *game_now);
            if (fixed == 0) {  // a repeated position sample
                g_dropped_repeats.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            Timing& t = g_timing[(key & 0xFFFF0000u) == 0xB01F0000u ? 1 : 0];
            if (t.wait.size() < 4096) {
                t.wait.push_back(static_cast<int64_t>(now - due));
                t.margin.push_back(static_cast<int64_t>(fixed) -
                                   (static_cast<int64_t>(*game_now) - kClientRenderDelayMs));
            }
            if (fixed != ts) {
                std::memcpy(out.data() + ts_at, &fixed, 8);
                g_restamped.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (out.size() >= 5 + 6 && out[0] == (off::kOpRemoveActorAck & 0xFF) &&
                   out[1] == (off::kOpRemoveActorAck >> 8)) {
            // 04 | u32 | u8 reason | n x u32 key: those buffers are gone.
            for (size_t i = 5 + 6; i + 4 <= out.size(); i += 4) {
                uint32_t key;
                std::memcpy(&key, out.data() + i, 4);
                g_inject_clock.forget(key);
            }
        }
        has = true;
        break;
    }
    if (has) {
        // MoveActorAck: u32 key | f32 x, y, z | ...
        if (out.size() >= 5 + 16 && out[0] == (off::kOpMoveActorAck & 0xFF) && out[1] == (off::kOpMoveActorAck >> 8)) {
            uint32_t key;
            std::memcpy(&key, out.data() + 5, 4);
            if (key == kPuppetPlayer || key == kPuppetBody) {
                std::memcpy(g_told_pos, out.data() + 9, sizeof g_told_pos);
                g_told_x.store(g_told_pos[0], std::memory_order_relaxed);
                g_told_z.store(g_told_pos[2], std::memory_order_relaxed);
                g_told_ms = now;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_queue_lock);
    if (has) fill_rope_piece(out);
    return has;
}

bool puppet_install_layer_guards() {
    if (g_layer_guard_hook && g_layers_skip_hook) return true;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    {
        uint8_t h[sizeof off::kActionLayerAdvanceHead];
        if (game::safe_read(base + off::kActionLayerAdvance, h, sizeof h) &&
            std::memcmp(h, off::kActionLayerAdvanceHead, sizeof h) == 0) {
            g_ret_stub = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (g_ret_stub) {
                g_ret_stub[0] = 0xC3;
                DWORD old = 0;
                VirtualProtect(g_ret_stub, 16, PAGE_EXECUTE_READ, &old);
                FlushInstructionCache(GetCurrentProcess(), g_ret_stub, 16);
                auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(base + off::kActionLayerAdvance),
                                                        action_layer_advance);
                if (hook) g_layer_guard_hook = std::move(*hook);
                uint8_t h2[sizeof off::kActionLayersAdvanceHead];
                if (game::safe_read(base + off::kActionLayersAdvance, h2, sizeof h2) &&
                    std::memcmp(h2, off::kActionLayersAdvanceHead, sizeof h2) == 0) {
                    auto skip = safetyhook::MidHook::create(
                        reinterpret_cast<void*>(base + off::kActionLayersAdvance), action_layers_advance);
                    if (skip) g_layers_skip_hook = std::move(*skip);
                }
            }
        }
        if (!g_layer_guard_hook || !g_layers_skip_hook) {
            g_layer_guard_hook = {};
            g_layers_skip_hook = {};
            return false;
        }
        log::info("Puppet: action-layer crash guard installed");
    }
    return true;
}

bool puppet_install_climb_fix() {
    if (g_climb_enter_hook) return true;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t enter = base + off::kPointClimbEnter;
    uint8_t head[sizeof off::kPointClimbEnterHead];
    if (base != off::kImageBase || !game::safe_read(enter, head, sizeof head) ||
        std::memcmp(head, off::kPointClimbEnterHead, sizeof head) != 0) {
        log::warn("Puppet: PointClimb::Enter not recognised; wall climbs will freeze");
        return false;
    }
    g_climb_enter_hook = safetyhook::create_inline(reinterpret_cast<void*>(enter),
                                                   reinterpret_cast<void*>(point_climb_enter));
    const bool ok = static_cast<bool>(g_climb_enter_hook);
    log::info("Puppet: wall-climb fix {}", ok ? "installed" : "FAILED to install");
    // Mount / dismount (same idea). Both share one prologue. Only with the
    // horse: without one, the copy stayed stuck in the dismount pose.
    if (!g_horse_enabled) return ok;
    // A horse copy must never load without the guard (caballo-coop.md §9.4 R1).
    if (!puppet_install_layer_guards()) {
        g_horse_enabled = false;
        log::warn("Puppet: action-layer crash guard not installed; horse copy disabled");
        return ok;
    }
    for (auto [rva, hook, fn, name] :
         {std::tuple{off::kRideOnEnter, &g_ride_on_hook, reinterpret_cast<void*>(ride_on_enter), "RideOn"},
         }) {
        uint8_t h[sizeof off::kRideEnterHead];
        if (!game::safe_read(base + rva, h, sizeof h) || std::memcmp(h, off::kRideEnterHead, sizeof h) != 0) {
            log::warn("Puppet: {}::Enter not recognised; mounting may freeze the other player", name);
            continue;
        }
        *hook = safetyhook::create_inline(reinterpret_cast<void*>(base + rva), fn);
        log::info("Puppet: {} fix {}", name, *hook ? "installed" : "FAILED to install");
    }
    return ok;
}

void puppet_set_offset(float metres) {
    g_offset_x = metres;
    log::info("Puppet offset set to {:.2f} m", metres);
}

float puppet_offset_m() { return g_offset_x.load(); }

void puppet_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5 + 22) return;
    uint16_t op = 0;
    std::memcpy(&op, packet, 2);
    const uint8_t* p = packet + 5;
    if (op == off::kOpRemoteCatchResultAck && size >= 5 + kRemoteCatchPoint) {
        // Our own hook (our player, a scene object): that tree's piece id here.
        uint32_t catcher = 0;
        std::memcpy(&catcher, p, 4);
        const uint32_t own = capture_own_player_key();
        if (!own || catcher != own || p[9] != 2) return;
        SceneId id;
        std::memcpy(id.data(), p + off::kRemoteCatchSceneId, id.size());
        uint64_t piece = 0;
        std::memcpy(&piece, p + kRemoteCatchPiece, 8);
        std::lock_guard lock(g_gimmick_mutex);
        if (g_scene_gimmicks.count(id)) g_own_pieces[id] = piece;
        return;
    }
    if (size != 5 + 22 || op != off::kOpUpdateOnDemandGimmickAck || p[0] != 4) return;
    SceneId id;
    std::memcpy(id.data(), p + 6, id.size());
    std::lock_guard lock(g_gimmick_mutex);
    g_own_pieces.erase(id);
    if (p[5] == 1) {
        if (g_scene_gimmicks.size() >= 4096) g_scene_gimmicks.clear(), g_own_pieces.clear();
        g_scene_gimmicks.insert(id);
    } else {
        g_scene_gimmicks.erase(id);
    }
}

}  // namespace cdmp::net

namespace cdmp::net {
void puppet_set_horse_enabled(bool on) {
    g_horse_enabled = on;
    log::info("Puppet: horse {}", on ? "enabled (experimental)" : "disabled");
}

void puppet_set_equip_enabled(bool on) {
    g_equip_enabled = on;
    log::info("Puppet: equipment relay {}", on ? "enabled (experimental)" : "disabled");
}
}  // namespace cdmp::net
