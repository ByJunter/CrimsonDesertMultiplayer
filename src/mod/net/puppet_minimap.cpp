#include "mod/net/puppet_minimap.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <safetyhook.hpp>

#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/troc_sniffer.h"

namespace cdmp::net {
namespace {
namespace off = game::build_2976;

constexpr uint32_t kPuppetPlayer = 0xA010F001;

SafetyHookInline g_hook;
std::atomic<int> g_offset{0};

// What the hook saw, for the log (written on game threads).
std::atomic<uint64_t> g_calls{0};       // for the puppet
std::atomic<uint32_t> g_last_icon{0xFFFFFFFF};
std::atomic<uint32_t> g_diag_answer{0xFFFFFFFF};  // icon | sub << 16 (diagnostic)
std::atomic<uint32_t> g_diag_flags{0};            // a | b << 8
std::atomic<uint64_t> g_failed{0};      // icon table not readable
// Copies of the other player's enemies: the game gives a network-driven NPC
// no icon (never in battle, no aggro: 0x9E9754 falls to 0xFFFF), so they get
// the hostile one at this table offset when it gives none; a dead copy keeps
// the game's corpse icon (copias-enemigos-faltan-pose-minimapa.md §4).
std::atomic<int> g_enemy_offset{0};
std::atomic<uint64_t> g_enemy_calls{0}, g_enemy_given{0};
// The icons the game gives the real NPCs of this world (first ones seen), to
// find the hostile one.
std::atomic<uint32_t> g_npc_icons[8] = {};
std::atomic<uint32_t> g_copy_answer{0xFFFFFFFF};

bool is_enemy_copy(uint32_t key) { return (key & 0xFFFF0000u) == 0xB01F0000u && (key & 0xFFFFu) != 0; }

// An enemy copy's state, read without the game's minimap policy
// (docs/research/analisis-ia2/P1-minimapa.md): the game's own answer ("b",
// no icon) mixes AI, sequencer and map policies and was set for every copy
// (7 Oct night: 397 of 397), so it cannot tell a hidden copy.
// Status = [[actor+0x68]+0x20]: u8 +0x273 dead, u8 +0x340 no icon, u8 +0x341
// map hide, u32 +0x390/+0x394 physical hide flags; model alpha =
// f32 [[[actor+0x68]+0x40]+0xE8]+0x100; actor u16 +0x5E state, u8 +0xCF login.
struct CopyState {
    bool valid = false, dead = false, no_icon = false, map_hide = false, ready = false;
    uint32_t flags = 0;
    float alpha = NAN;  // NaN: no model readable
    // The minimap policy's own inputs (analisis-ia2/P1 §3.2), for the probe:
    int ai_bit = -1;     // bit 0x04000000 of u64 [[[comps+0x58]+0x60]+0x38]
    int seq3dc = -1;     // u8 [[comps+0xB0]+0x3DC] (the sequencer, not TransformSync +0x1A0)
    int faction4a0 = -1; // Status +0x4A0 & 1
};
CopyState read_copy_state(const void* comp) {
    CopyState s;
    __try {
        const auto actor = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(comp) + 0x08);
        const auto comps = actor ? *reinterpret_cast<const uintptr_t*>(actor + 0x68) : 0;
        const auto status = comps ? *reinterpret_cast<const uintptr_t*>(comps + 0x20) : 0;
        if (!status) return s;
        s.dead = *reinterpret_cast<const uint8_t*>(status + 0x273) != 0;
        s.no_icon = *reinterpret_cast<const uint8_t*>(status + 0x340) != 0;
        s.map_hide = *reinterpret_cast<const uint8_t*>(status + 0x341) != 0;
        s.flags = *reinterpret_cast<const uint32_t*>(status + 0x390) | *reinterpret_cast<const uint32_t*>(status + 0x394);
        s.ready = *reinterpret_cast<const uint16_t*>(actor + 0x5E) == 0x10 && *reinterpret_cast<const uint8_t*>(actor + 0xCF) != 0;
        s.valid = true;
        const auto control = *reinterpret_cast<const uintptr_t*>(comps + 0x40);
        const auto model = control ? *reinterpret_cast<const uintptr_t*>(control + 0xE8) : 0;
        if (model) s.alpha = *reinterpret_cast<const float*>(model + 0x100);
        s.faction4a0 = *reinterpret_cast<const uint8_t*>(status + 0x4A0) & 1;
        if (const auto ai = *reinterpret_cast<const uintptr_t*>(comps + 0x58)) {
            if (const auto ai60 = *reinterpret_cast<const uintptr_t*>(ai + 0x60)) {
                s.ai_bit = (*reinterpret_cast<const uint64_t*>(ai60 + 0x38) & 0x04000000u) ? 1 : 0;
            }
        }
        if (const auto seq = *reinterpret_cast<const uintptr_t*>(comps + 0xB0)) {
            s.seq3dc = *reinterpret_cast<const uint8_t*>(seq + 0x3DC);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.valid = false;
    }
    return s;
}

// Why a copy kept the game's answer.
enum Kept { kKeptDead, kKeptNoIcon, kKeptHidden, kKeptNotReady, kKeptFault, kKeptLoading, kKeptCount };
std::atomic<uint64_t> g_kept_why[kKeptCount];
constexpr const char* kKeptWhy[kKeptCount] = {"dead", "+340", "hidden (flags)", "not ready", "fault", "loading"};

// The probe (P1 §7): one line per copy whenever what we read or decided
// changes, written by puppet_minimap_tick (never log I/O on the game's thread).
SRWLOCK g_probe_lock = SRWLOCK_INIT;
std::map<uint32_t, std::string> g_probe_last;  // copy -> its last line's body
std::vector<std::string> g_probe_lines;
uint64_t g_probe_count = 0;
constexpr uint64_t kMaxProbeLines = 200;

void probe(uint32_t key, const CopyState& s, const char* decision, uint16_t answer) {
    std::string body = std::format(
        "decision {} | answer {} | ready {} dead {} +340 {} +341 {} flags 0x{:X} alpha {:.2f} | ai bit26 {} "
        "seq+3DC {} +4A0&1 {}",
        decision, answer == 0xFFFF ? std::string("-") : std::to_string(answer), s.ready, s.dead, s.no_icon,
        s.map_hide, s.flags, s.alpha, s.ai_bit, s.seq3dc, s.faction4a0);
    if (!TryAcquireSRWLockExclusive(&g_probe_lock)) return;
    auto& last = g_probe_last[key];
    if (last != body && g_probe_count < kMaxProbeLines) {
        ++g_probe_count;
        g_probe_lines.push_back(std::format("Minimap probe: copy 0x{:08X} | {}", key, body));
        last = std::move(body);
    }
    if (g_probe_last.size() > 512) g_probe_last.clear();
    ReleaseSRWLockExclusive(&g_probe_lock);
}
std::atomic<uint64_t> g_given_map_hide{0};  // given although Status+0x341 was set (to learn what it means)
std::atomic<uint64_t> g_not_ready_given{0}, g_alpha0_given{0};  // given while loading / alpha 0

bool write_icon(uint16_t* icon, uint16_t* sub, uint8_t* a, uint8_t* b, uint16_t key) {
    __try {
        *icon = key;
        *sub = 0xFFFF;
        *a = 0;
        *b = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void note_npc_icon(uint16_t icon) {
    if (icon == 0xFFFF) return;
    for (auto& slot : g_npc_icons) {
        uint32_t v = slot.load(std::memory_order_relaxed);
        if ((v & 0xFFFF) == icon && v) {
            slot.store(v + 0x10000, std::memory_order_relaxed);  // count in the high half
            return;
        }
        if (v == 0) {
            uint32_t expected = 0;
            if (slot.compare_exchange_strong(expected, 0x10000u | icon)) return;
        }
    }
}

uint32_t actor_key_of(const void* comp) {
    __try {
        const auto actor = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(comp) + 0x08);
        if (!actor) return 0;
        return *reinterpret_cast<const uint32_t*>(actor + off::kActor_Key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// cfg = [[[[exe+0x6D69528]]+8]+0x270], a table of u16 UIMapIconKey.
uint16_t icon_from_table(int offset) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (uintptr_t root : {off::kMinimapIconRoot, off::kMinimapIconRootAlt}) {
        __try {
            uintptr_t p = *reinterpret_cast<const uintptr_t*>(base + root);
            if (!p) continue;
            p = *reinterpret_cast<const uintptr_t*>(p);
            if (!p) continue;
            p = *reinterpret_cast<const uintptr_t*>(p + 0x08);
            if (!p) continue;
            p = *reinterpret_cast<const uintptr_t*>(p + 0x270);
            if (!p) continue;
            return *reinterpret_cast<const uint16_t*>(p + offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 0xFFFF;
}

void __fastcall minimap_icon(void* comp, uint16_t* icon, uint16_t* sub, uint8_t* a, uint8_t* b) {
    const int offset = g_offset.load(std::memory_order_relaxed);
    const uint32_t actor = actor_key_of(comp);
    if (actor != kPuppetPlayer && (actor >> 28) == 0xB) {
        const int enemy = g_enemy_offset.load(std::memory_order_relaxed);
        if (is_enemy_copy(actor) && enemy > 0) {
            g_enemy_calls.fetch_add(1, std::memory_order_relaxed);
            // A live, loaded, shown copy: our icon, answered BEFORE the
            // original (which would also queue HideFlagMinimapActor for it).
            const CopyState s = read_copy_state(comp);
            int kept = -1;
            // While the world loads nothing is forced (analisis-ia2/P1 and P5).
            if (sniffer_world_loading()) kept = kKeptLoading;
            else if (!s.valid) kept = kKeptFault;
            else if (s.dead) kept = kKeptDead;
            else if (s.no_icon) kept = kKeptNoIcon;
            else if (s.flags != 0) kept = kKeptHidden;
            // Not ready or alpha 0 (fading in at load) still get it: a first
            // "none" may stop the game asking again for that copy (P1 §5).
            if (kept < 0 && !s.ready) g_not_ready_given.fetch_add(1, std::memory_order_relaxed);
            if (kept < 0 && s.alpha == s.alpha && s.alpha <= 0.0f) g_alpha0_given.fetch_add(1, std::memory_order_relaxed);
            if (kept < 0) {
                const uint16_t key = icon_from_table(enemy);
                if (key != 0xFFFF && write_icon(icon, sub, a, b, key)) {
                    g_enemy_given.fetch_add(1, std::memory_order_relaxed);
                    if (s.map_hide) g_given_map_hide.fetch_add(1, std::memory_order_relaxed);
                    probe(actor, s, "ours", key);
                    return;
                }
                kept = kKeptFault;
            }
            g_kept_why[kept].fetch_add(1, std::memory_order_relaxed);
            // Dead (corpse/loot icon), hidden, not ready: the game decides.
            g_hook.call<void>(comp, icon, sub, a, b);
            uint16_t answer = 0xFFFF;
            __try {
                answer = *icon;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            g_copy_answer.store(answer, std::memory_order_relaxed);
            probe(actor, s, kKeptWhy[kept], answer);
            return;
        }
        g_hook.call<void>(comp, icon, sub, a, b);
        uint16_t given = 0xFFFF;
        __try {
            given = *icon;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (!is_enemy_copy(actor)) note_npc_icon(given);
        return;
    }
    if (offset == 0 || actor != kPuppetPlayer) {
        g_hook.call<void>(comp, icon, sub, a, b);
        return;
    }
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (offset < 0) {  // diagnostic: what does the game decide for it?
        g_hook.call<void>(comp, icon, sub, a, b);
        __try {
            g_diag_answer.store(*icon | static_cast<uint32_t>(*sub) << 16, std::memory_order_relaxed);
            g_diag_flags.store(*a | static_cast<uint32_t>(*b) << 8, std::memory_order_relaxed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    const uint16_t key = icon_from_table(offset);
    if (key == 0xFFFF) {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        g_hook.call<void>(comp, icon, sub, a, b);
        return;
    }
    // Answer without the original: it would also send HideFlagMinimapActor
    // for an actor it considers hidden.
    __try {
        *icon = key;
        *sub = 0xFFFF;
        *a = 0;
        *b = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_last_icon.store(key, std::memory_order_relaxed);
}

}  // namespace

bool puppet_minimap_install(int table_offset) {
    if (table_offset == 0) return false;
    if (g_hook) {
        puppet_minimap_set_offset(table_offset);
        return true;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t head[sizeof off::kMinimapIconDecideHead];
    if (base != off::kImageBase || !game::safe_read(base + off::kMinimapIconDecide, head, sizeof head) ||
        std::memcmp(head, off::kMinimapIconDecideHead, sizeof head) != 0) {
        log::warn("Minimap: icon function not recognised; the other player gets no minimap icon");
        return false;
    }
    puppet_minimap_set_offset(table_offset);
    g_hook = safetyhook::create_inline(reinterpret_cast<void*>(base + off::kMinimapIconDecide),
                                       reinterpret_cast<void*>(minimap_icon));
    log::info("Minimap: icon hook {}", g_hook ? "installed" : "FAILED to install");
    return static_cast<bool>(g_hook);
}

void puppet_minimap_set_offset(int table_offset) {
    if (table_offset > 0 && (table_offset < 0xEA || table_offset > 0x16C || table_offset % 2)) return;
    g_offset = table_offset;
    if (table_offset > 0) {
        log::info("Minimap: other player's icon = table +0x{:X} (key {})", table_offset,
                  icon_from_table(table_offset));
    } else if (table_offset < 0) {
        log::info("Minimap: diagnostic mode (the game decides the other player's icon)");
    }
}

int puppet_minimap_offset() { return g_offset.load(); }

void puppet_minimap_set_enemy_offset(int table_offset) {
    if (table_offset > 0 && (table_offset < 0xEA || table_offset > 0x16C || table_offset % 2)) return;
    g_enemy_offset = table_offset;
    log::info("Minimap: the other player's enemy copies {}", table_offset > 0
                  ? std::format("get icon table +0x{:X} (key {}) when the game gives none", table_offset,
                                icon_from_table(table_offset))
                  : std::string("get what the game gives"));
}

void puppet_minimap_tick() {
    {
        std::vector<std::string> lines;
        AcquireSRWLockExclusive(&g_probe_lock);
        lines.swap(g_probe_lines);
        ReleaseSRWLockExclusive(&g_probe_lock);
        for (const auto& l : lines) log::info("{}", l);
    }
    static uint64_t logged_calls = 0, logged_failed = 0;
    static uint32_t logged_answer = 0xFFFFFFFE, logged_flags = 0xFFFFFFFF, logged_icon = 0xFFFFFFFE;
    const uint64_t calls = g_calls.load(std::memory_order_relaxed);
    const uint32_t answer = g_diag_answer.load(std::memory_order_relaxed);
    const uint32_t flags = g_diag_flags.load(std::memory_order_relaxed);
    const uint32_t icon = g_last_icon.load(std::memory_order_relaxed);
    if (calls && !logged_calls) log::info("Minimap: the game asked for the other player's icon");
    if (calls) logged_calls = calls;
    if (answer != 0xFFFFFFFF && (answer != logged_answer || flags != logged_flags)) {
        log::info("Minimap (diagnostic): the game gives the other player icon {} sub {} flags {} {}",
                  answer & 0xFFFF, answer >> 16, flags & 0xFF, flags >> 8);
        logged_answer = answer;
        logged_flags = flags;
    }
    if (icon != 0xFFFFFFFF && icon != logged_icon) {
        log::info("Minimap: other player shown with icon {}", icon);
        logged_icon = icon;
    }
    static uint64_t logged_enemy = 0, last_npc_log = 0;
    if (const uint64_t ec = g_enemy_calls.load(std::memory_order_relaxed); ec && GetTickCount64() - last_npc_log >= 60000) {
        last_npc_log = GetTickCount64();
        std::string npcs;
        for (const auto& slot : g_npc_icons) {
            if (const uint32_t v = slot.load(std::memory_order_relaxed)) npcs += std::format(" {}x{}", v & 0xFFFF, v >> 16);
        }
        if (ec != logged_enemy) {
            log::info("Minimap: enemy copies asked {} times, given our icon {} times (the game's last answer {}) | "
                      "icons of this world's NPCs:{} | table +0x10A {} +0x10C {} +0x110 {} +0x112 {} +0x114 {} | "
                      "kept the game's:{}",
                      ec, g_enemy_given.load(), g_copy_answer.load() & 0xFFFF, npcs, icon_from_table(0x10A),
                      icon_from_table(0x10C), icon_from_table(0x110),
                      icon_from_table(0x112), icon_from_table(0x114),
                      [] {
                          std::string t;
                          for (int k = 0; k < kKeptCount; ++k) {
                              if (const uint64_t n = g_kept_why[k].load()) t += std::format(" {} {}", kKeptWhy[k], n);
                          }
                          if (const uint64_t m = g_given_map_hide.load()) t += std::format(" | given with +341 set {}", m);
                          if (const uint64_t m = g_not_ready_given.load()) t += std::format(" | given not ready {}", m);
                          if (const uint64_t m = g_alpha0_given.load()) t += std::format(" | given at alpha 0 {}", m);
                          return t.empty() ? std::string(" none") : t;
                      }());
            logged_enemy = ec;
        }
    }
    const uint64_t failed = g_failed.load(std::memory_order_relaxed);
    if (failed != logged_failed) {
        log::info("Minimap: icon table not readable ({} times)", failed);
        logged_failed = failed;
    }
}

}  // namespace cdmp::net
