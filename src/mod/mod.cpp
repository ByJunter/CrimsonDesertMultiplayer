#include "mod/mod.h"

#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "build_id.h"
#include "common/client_session.h"
#include "common/clock.h"
#include "common/transport.h"
#include "common/udp.h"
#include "mod/config.h"
#include "mod/game/game_errors.h"
#include "mod/game/offsets.h"
#include "mod/game/player_reader.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/coop.h"
#include "mod/net/aggro_test.h"
#include "mod/net/attack_probe.h"
#include "mod/net/enemy_relay.h"
#include "mod/net/horse_diag.h"
#include "mod/net/local_capture.h"
#include "mod/net/guest_hits.h"
#include "mod/net/loot_twin.h"
#include "mod/net/learnt_store.h"
#include "mod/net/native_suppress.h"
#include "mod/net/piece4_test.h"
#include "mod/net/player_hits.h"
#include "mod/net/puppet_minimap.h"
#include "mod/net/remote_puppet.h"
#include "mod/net/steam_transport.h"
#include "mod/net/time_sync.h"
#include "mod/net/troc_sniffer.h"
#include "mod/render/dx12_hook.h"
#include "mod/render/hud.h"

#pragma comment(lib, "version.lib")

namespace cdmp {
namespace {

// The commit this build comes from (build_id.h): both players must match.
constexpr uint32_t kModBuild = CDMP_BUILD_ID;

HMODULE g_self = nullptr;
HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_late_thread = nullptr;

bool stop_requested(DWORD wait_ms = 0) {
    return WaitForSingleObject(g_stop, wait_ms) == WAIT_OBJECT_0;
}

std::wstring module_path(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring dir_of(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::string file_version(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return "unknown";
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return "unknown";
    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

// Facts that identify the exact game build. Every offset/signature we find
// later is only valid for one build, so the log must always say which one.
bool log_process_info() {
    const std::wstring exe = module_path(nullptr);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    log::info("Host process: {} (PID {})", narrow(exe), GetCurrentProcessId());
    log::info("Host version: {}  image base: 0x{:X}  image size: 0x{:X}  PE timestamp: 0x{:08X}",
              file_version(exe), base, nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp);

    const bool in_game = exe.ends_with(L"\\CrimsonDesert.exe");
    log::info("{}", in_game ? "Running inside CrimsonDesert.exe"
                            : "Not running inside the game (test host); game features stay off");

    if (HMODULE winmm = GetModuleHandleW(L"winmm.dll")) {
        log::info("winmm.dll loaded from: {}", narrow(module_path(winmm)));
    }
    return in_game;
}

// Outside the game (test host) there is no player to read, so we walk a 2 m
// circle instead, flagged as synthetic.
proto::PlayerState synthetic_state(uint32_t t_ms) {
    const float angle = static_cast<float>(t_ms % 8000) / 8000.0f * 6.2831853f;
    proto::PlayerState s;
    s.pos = {2.0f * std::cos(angle), 0.0f, 2.0f * std::sin(angle)};
    s.rot = {0.0f, std::sin(angle * 0.5f), 0.0f, std::cos(angle * 0.5f)};
    s.flags = proto::kStateSynthetic;
    return s;
}

// Where the local player's state comes from: game memory inside the game,
// the synthetic circle in the test host.
class LocalSource {
public:
    explicit LocalSource(bool in_game) : in_game_(in_game) {
        if (!in_game_) return;
        if (reader_.init()) {
            log::info("Game reader enabled (build 1.0.0.2976 offsets)");
        } else {
            log::warn("Game reader DISABLED: {}. Only networking will run.",
                      reader_.last_failure());
        }
    }

    // nullopt while the player is not in the world: nothing is sent then.
    std::optional<proto::PlayerState> sample(uint32_t now) {
        if (!in_game_) return synthetic_state(now);

        last_ = reader_.read();
        // Log only transitions, not every failed 20 Hz read.
        const std::string status = last_ ? "in world" : reader_.last_failure();
        if (status != status_) {
            log::info("Local player: {}", status);
            status_ = status;
        }
        if (!last_) return std::nullopt;

        proto::PlayerState s;
        s.pos = last_->position;
        s.rot = last_->rotation;
        s.health = last_->health;
        s.health_max = last_->health_max;
        return s;
    }

    const std::optional<game::PlayerSnapshot>& snapshot() const { return last_; }
    bool in_game() const { return in_game_; }
    const std::string& status() const { return status_; }

    std::string describe() const {
        if (!in_game_) return "synthetic";
        if (!last_) return "unavailable (" + status_ + ")";
        const auto& p = *last_;
        return std::format("pos ({:.2f}, {:.2f}, {:.2f}) rot ({:.3f}, {:.3f}, {:.3f}, {:.3f}) "
                           "hp {:.1f}/{:.1f} stamina {:.1f}/{:.1f} | check vs game global {:.2f} m",
                           p.position.x, p.position.y, p.position.z, p.rotation.x, p.rotation.y,
                           p.rotation.z, p.rotation.w, p.health, p.health_max, p.stamina,
                           p.stamina_max, p.static_pos_error);
    }

private:
    bool in_game_;
    game::PlayerReader reader_;
    std::optional<game::PlayerSnapshot> last_;
    std::string status_;
};

void run_network(const Config& cfg, LocalSource& local) {
    if (!net::startup()) {
        log::error("Winsock startup failed");
        return;
    }

    // A guest knows whom to talk to; a host learns it from the first Hello.
    const bool host = cfg.net_role_host;
    std::unique_ptr<net::Transport> transport;
    net::SteamTransport* steam = nullptr;
    std::optional<net::PeerAddr> peer;
    if (cfg.net_transport_steam) {
        if (!local.in_game()) {
            log::error("The Steam transport only works inside the game");
            net::cleanup();
            return;
        }
        auto st = std::make_unique<net::SteamTransport>(cfg.steam_channel, host, cfg.steam_peer_id);
        steam = st.get();
        transport = std::move(st);
        if (!host && cfg.steam_peer_id) {
            peer = net::PeerAddr::steam(cfg.steam_peer_id);
            steam->set_peer(cfg.steam_peer_id);
        }
    } else {
        // UDP is only for tests on one PC (cdmp_peer, cdmp_host): it never
        // leaves this computer. Between players only Steam is used: it hides
        // the IPs, encrypts, and the host accepts only friends.
        auto udp = std::make_unique<net::UdpTransport>();
        bool ok = false;
        if (host) {
            ok = udp->open(cfg.peer_port, /*loopback_only=*/true);
        } else if (auto ep = net::Endpoint::resolve(cfg.peer_host, cfg.peer_port)) {
            if (!ep->is_loopback()) {
                log::error("UDP only connects within this PC (tests); to play with someone use transport=steam");
                net::cleanup();
                return;
            }
            peer = net::PeerAddr::udp(*ep);
            ok = udp->open(0, true);
        } else {
            log::error("Cannot resolve peer host '{}'", cfg.peer_host);
            net::cleanup();
            return;
        }
        if (!ok) {
            log::error("Cannot open UDP socket: {}", net::UdpSocket::last_error());
            net::cleanup();
            return;
        }
        transport = std::move(udp);
    }
    log::info("Network: {} as {}; peer {}", transport->status(), host ? "host" : "guest",
              peer ? peer->str() : (host ? "= whoever connects" : "= the only friend playing"));

    ClientSession session(
        cfg.player_name, kModBuild,
        [&](const std::vector<uint8_t>& bytes) {
            if (peer) transport->send(*peer, bytes);
        },
        [](const std::string& text) { log::info("Session: {}", text); },
        host ? ClientSession::Role::Responder : ClientSession::Role::Initiator);
    bool was_connected = false;
    uint32_t peer_since = 0;  // host: when it locked onto its player
    uint32_t last_friend_scan = 0;
    size_t last_friend_count = SIZE_MAX;

    std::unique_ptr<net::CoopLink> coop;
    if (cfg.coop_enabled && local.in_game()) {
        coop = std::make_unique<net::CoopLink>(session, cfg.coop_delay_ms, cfg.coop_offset_m,
                                               cfg.coop_map_pin);
        // A real other player (Steam): each side only takes what the other role sends.
        coop->set_strict_roles(steam != nullptr);
    }

    const uint32_t state_interval = 1000 / cfg.send_rate_hz;
    uint32_t last_state = 0;
    uint32_t last_report = now_ms();
    // Ctrl+F12 writes a numbered mark in the log: both players press it at the
    // same moment ("now!") so their logs can be lined up afterwards.
    bool mark_was_down = false;
    int marks = 0;
    std::vector<uint8_t> buf(proto::kMaxPacketSize);

    while (!stop_requested()) {
        transport->poll(now_ms());

        // Steam guest without a configured host: the only friend playing.
        if (steam && !host && !peer && steam->ready() && now_ms() - last_friend_scan >= 2000) {
            last_friend_scan = now_ms();
            const auto playing = steam->friends_playing();
            if (playing.size() == 1) {
                peer = net::PeerAddr::steam(playing[0].id);
                steam->set_peer(playing[0].id);
                log::info("Network: connecting to '{}' (the only friend playing)", playing[0].name);
            } else if (playing.size() != last_friend_count) {
                last_friend_count = playing.size();
                log::info("Network: {} friends playing; put the host's SteamID64 in [steam] "
                          "peer_steamid (listed above when Steam got ready)",
                          playing.size());
            }
        }

        // Everything that is waiting, not one datagram per loop: with ~100
        // events/s (a busy city) handling one per loop built a backlog of
        // seconds in the first real test (RTT up to 2.9 s).
        net::PeerAddr from;
        int timeout = 5;
        for (int handled = 0; handled < 512; ++handled) {
            auto n = transport->recv(from, buf, timeout);
            if (!n) break;
            timeout = 0;
            if (host && !peer) {
                // Lock onto the first player who greets us.
                // Only a Hello of our protocol: one of another version would
                // never connect and would keep the place (security review M2).
                auto packet = proto::decode({buf.data(), *n});
                if (packet && packet->version == proto::kVersion &&
                    std::holds_alternative<proto::Hello>(packet->body)) {
                    peer = from;
                    peer_since = now_ms();
                    if (steam) steam->set_peer(from.value);
                    log::info("Network: player at {} is joining", from.str());
                }
            }
            if (peer && from == *peer) session.on_datagram({buf.data(), *n}, now_ms());
        }

        const uint32_t now = now_ms();
        session.tick(now);
        const bool connected = session.state() == ClientSession::State::Connected;
        if (host && was_connected && !connected && peer) {
            log::info("Network: {} left; waiting for a player", peer->str());
            peer.reset();
            if (steam) steam->set_peer(0);
        } else if (host && !was_connected && !connected && peer && now - peer_since > 5000) {
            log::info("Network: {} did not complete the handshake; waiting for a player", peer->str());
            peer.reset();
            if (steam) steam->set_peer(0);
        }
        was_connected = connected;
        if (coop) {
            std::optional<proto::Vec3> pos;
            proto::Quat rot;
            if (local.snapshot()) {
                pos = local.snapshot()->position;
                rot = local.snapshot()->rotation;
            }
            coop->update(pos, rot, now);
        }

        if (now - last_state >= state_interval) {
            last_state = now;
            if (auto state = local.sample(now)) session.send_local_state(*state, now);

            render::HudData hud;
            hud.connected = session.state() == ClientSession::State::Connected;
            hud.peer_name = session.peer_name();
            hud.rtt_ms = session.stats().rtt_ms;
            hud.local = local.snapshot();
            hud.local_status = local.status();
            hud.remote = session.remote_state();
            hud.peer_problem = session.peer_problem();
            // In co-op, put the marker on the character actually drawn (it
            // plays the network state `delay` ms later).
            if (coop && hud.remote) {
                if (auto pos = net::puppet_position()) hud.remote->pos = *pos;
            }
            if (coop) hud.enemy_zone = net::suppress_hud_text();
            render::hud_update(std::move(hud));
        }

        const bool mark_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (mark_down && !mark_was_down) log::warn("=== MARK {} (Ctrl+F12) ===", ++marks);
        mark_was_down = mark_down;
        if (now - last_report >= 5000) {
            last_report = now;
            const auto& st = session.stats();
            const char* state =
                session.state() == ClientSession::State::Connected ? "connected" : "connecting";
            std::string remote = "none";
            if (const auto& rs = session.remote_state()) {
                remote = std::format("({:.2f}, {:.2f}, {:.2f}) hp {:.1f}", rs->pos.x, rs->pos.y,
                                     rs->pos.z, rs->health);
            }
            log::info("Net: {} | sent {} recv {} rejected {} | rtt {:.1f} ms | remote states {} | "
                      "last remote {} | reliable pending {} refused {} | unknown kinds {} | {}",
                      state, st.sent, st.received, st.rejected, st.rtt_ms, st.remote_states, remote,
                      session.reliable_pending(), session.reliable_refused(), st.unknown_kinds,
                      transport->status());
            if (!session.peer_problem().empty()) log::warn("Net: {}", session.peer_problem());
            log::info("Local: {}", local.describe());
            if (coop) log::info("{}", coop->describe());
            if (cfg.overlay_enabled) {
                const auto ov = render::status();
                log::info("Overlay: installed {} disabled {} | presents {} drawn {} | queue {}{}",
                          ov.installed, ov.disabled, ov.presents, ov.frames_drawn,
                          ov.queue_source.empty() ? "-" : ov.queue_source,
                          ov.last_error.empty() ? "" : " | error: " + ov.last_error);
            }
        }
    }

    coop.reset();
    session.disconnect(now_ms());
    transport.reset();
    net::cleanup();
    log::info("Network stopped");
}

// The game's own top-level window, once it is visible and reasonably large.
bool game_window_visible() {
    struct Ctx {
        DWORD pid;
        bool found;
    } ctx{GetCurrentProcessId(), false};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            RECT r{};
            if (pid == c->pid && IsWindowVisible(hwnd) && GetClientRect(hwnd, &r) &&
                r.right - r.left >= 640 && r.bottom - r.top >= 360) {
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

struct LateInit {
    bool in_game;
    bool overlay;
    bool sniffer;
    bool clone_hotkeys;
    uint32_t mirror_delay_ms;
    float mirror_offset_m;
    bool coop;
    float coop_offset_m;
    bool coop_horse;
    bool coop_equip;
    bool coop_damage;
    bool combat_tests;
    bool record;
    std::wstring dir;
    int minimap_icon;
    bool minimap_cycle;
    bool aggro_test;
    bool enemies_target_copy;
    bool enemy_relay;
    bool piece4_test;
    bool guest_hits, guest_hit_stagger, player_hit_react, player_hit_lethal;
    bool attack_probe;
    int copy_attack_slots;
    uint8_t pin_shape, pin_colour;
};

// Dev: Ctrl+F9 / Ctrl+Shift+F9 walk the game's icon table for the other
// player's minimap icon. The game only draws a changed icon if it creates it
// anew (live test: changing it in place made it vanish), so each step
// reloads the copy.
void minimap_cycle_loop() {
    log::info("Minimap: Ctrl+F9 / Ctrl+Shift+F9 change the other player's icon");
    bool was_down = false;
    while (!stop_requested(50)) {
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool down = ctrl && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was_down) {
            const bool back = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            int at = net::puppet_minimap_offset();
            if (at < 0xEA || at > 0x16C) at = game::build_2976::kMinimapIconPlayerArrow;
            else at += back ? -2 : 2;
            if (at > 0x16C) at = 0xEA;
            if (at < 0xEA) at = 0x16C;
            net::puppet_minimap_set_offset(at);
            net::puppet_request_reload();
        }
        was_down = down;
        net::puppet_minimap_tick();
    }
}

// Combat injection tests (dev, combate-coop.md §6). They post client->server
// packets into the game's own server, next to the nearest NPC:
//   Ctrl+F6  T2: VaryStatByAccInputReq(npc, HP, -5)      does HP drop? death? loot?
//   Ctrl+F7  T3: AttackByCollisionReq(npc, player, ...)  native hit reaction/damage?
// Results show in cdmp_packets.log (VaryHPAck, AttackTargetAck, NotifyHandleDeadAck...).
void combat_test_loop() {
    log::warn("Combat tests ON: Ctrl+F6 = T2 (HP -5 on nearest NPC), Ctrl+F7 = T3 (collision hit). "
              "These WRITE to the game: do not save afterwards");
    game::PlayerReader reader;
    reader.init();
    bool f6_was = false, f7_was = false;
    auto packet = [](uint16_t op, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> p(5 + payload.size());
        const uint16_t len = static_cast<uint16_t>(payload.size());
        std::memcpy(p.data(), &op, 2);
        p[2] = 0;  // slot 0, as in every captured Req
        std::memcpy(p.data() + 3, &len, 2);
        std::memcpy(p.data() + 5, payload.data(), payload.size());
        return p;
    };
    while (!stop_requested(50)) {
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool f6 = ctrl && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        const bool f7 = ctrl && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if ((f6 && !f6_was) || (f7 && !f7_was)) {
            auto me = reader.read();
            auto npc = me ? net::capture_nearest_actor(me->position, 15.0f) : std::nullopt;
            // Prefer the enemy the player just hit (certainly has hit points).
            if (npc && net::capture_last_attack_target() && npc->key != net::capture_last_attack_target()) {
                log::info("Combat test: nearest enemy 0x{:08X} is not the last one you hit (0x{:08X}); "
                          "using the nearest",
                          npc->key, net::capture_last_attack_target());
            }
            if (!npc) {
                log::warn("Combat test: no enemy with hit points moving within 15 m (hit one first)");
            } else if (f6) {
                std::vector<uint8_t> b(16);
                const uint32_t hp_stat = 0x000F4240;
                const int64_t delta = -5000;  // -5 HP (x1000)
                std::memcpy(b.data(), &npc->key, 4);
                std::memcpy(b.data() + 4, &hp_stat, 4);
                std::memcpy(b.data() + 8, &delta, 8);
                const bool ok = net::sniffer_inject_c2s(packet(2181, b));
                log::info("Combat test T2: VaryStatByAccInputReq(0x{:08X} at {:.1f} m, HP, -5) {}",
                          npc->key, npc->distance, ok ? "queued" : "NOT queued");
            } else {
                // victim, attacker, u32, vec3 position, vec3 direction, u32 hash, u32.
                std::vector<uint8_t> b(44, 0);
                const uint32_t player = net::capture_own_player_key();
                uint32_t hash = net::capture_last_attack_hash();
                if (!hash) hash = 0xF2EE2678;  // a hash seen in captura 2
                const float pos[3] = {npc->pos.x, npc->pos.y + 1.0f, npc->pos.z};
                const float dx = npc->pos.x - me->position.x, dz = npc->pos.z - me->position.z;
                const float len = std::sqrt(dx * dx + dz * dz) + 1e-4f;
                const float dir[3] = {dx / len, 0.0f, dz / len};
                std::memcpy(b.data(), &npc->key, 4);
                std::memcpy(b.data() + 4, &player, 4);
                std::memcpy(b.data() + 12, pos, 12);
                std::memcpy(b.data() + 24, dir, 12);
                std::memcpy(b.data() + 36, &hash, 4);
                const bool ok = net::sniffer_inject_c2s(packet(2815, b));
                log::info("Combat test T3: AttackByCollisionReq(0x{:08X} at {:.1f} m, hash 0x{:08X}) {}",
                          npc->key, npc->distance, hash, ok ? "queued" : "NOT queued");
            }
        }
        f6_was = f6;
        f7_was = f7;
    }
}

// Single-PC mirror (dev): F9 spawns a copy of the player that replays the
// player's own events after `delay_ms`, F10 removes it, F11 pauses it. Same
// sender (local_capture) and receiver (remote_puppet) as co-op, minus the
// network and with the same game clock on both ends.
void mirror_hotkey_loop(uint32_t delay_ms) {
    log::info("Mirror hotkeys active: F9 = spawn your copy, F10 = remove it, "
              "F11 = mirroring on/off (delay {} ms)", delay_ms);
    auto mirror_sink = [delay_ms](const proto::GameEvent& ev) {
        if (ev.kind == proto::EventKind::Damage) return;  // same world: would double every hit
        net::puppet_event(ev, static_cast<int64_t>(delay_ms), delay_ms);
    };
    game::PlayerReader reader;
    reader.init();
    bool mirror_on = true;
    bool f9_was = false, f10_was = false, f11_was = false;
    while (!stop_requested(50)) {
        const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !f11_was) {
            mirror_on = !mirror_on;
            log::info("Mirror {}", mirror_on ? "ON" : "OFF");
            net::capture_set_sink(mirror_on && net::puppet_spawned() ? net::EventSink(mirror_sink)
                                                                     : net::EventSink{});
        }
        if (f9 && !f9_was) {
            auto me = reader.read();
            auto info = me ? net::capture_spawn_info(me->position, me->rotation) : std::nullopt;
            if (!me) {
                log::warn("F9: player not in world ({})", reader.last_failure());
            } else if (!info) {
                log::warn("F9: no templates yet (load a save with the sniffer on, then try again)");
            } else if (net::puppet_spawn(*info)) {
                if (mirror_on) net::capture_set_sink(mirror_sink);
            }
        }
        if (f10 && !f10_was) {
            net::capture_set_sink({});
            if (!net::puppet_remove()) log::warn("F10: nothing to remove");
        }
        f9_was = f9;
        f10_was = f10;
        f11_was = f11;
    }
    net::capture_set_sink({});
}

// Hooks are installed only once the game is fully up: installing the overlay
// at process start (creating our probe D3D12 device while the game and
// Streamline initialise theirs) left the game stuck on a black window.
DWORD WINAPI late_init(LPVOID param) {
    std::unique_ptr<LateInit> init(static_cast<LateInit*>(param));
    if (init->in_game) {
        log::info("Late init: waiting for the game to finish starting up");
        const auto world_slot = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) +
                                game::build_2976::kWorldSystemSlot;
        // WorldSystem exists from the title screen on, i.e. after the game
        // created its device and swap chain and started presenting.
        while (!stop_requested(500)) {
            uintptr_t world = 0;
            game::safe_read(world_slot, &world, sizeof world);
            if (world != 0 && game_window_visible()) break;
        }
        if (stop_requested(5000)) return 0;
        log::info("Late init: game is up, installing hooks");
    }
    bool sniffer_ok = false;
    if (init->sniffer && init->in_game) sniffer_ok = net::sniffer_install(init->dir, g_stop, init->record);
    if (sniffer_ok) game::game_errors_install();  // drained by the sniffer's writer thread
    if (init->overlay) render::install(render::hud_draw);
    if (init->combat_tests && sniffer_ok) {
        CloseHandle(CreateThread(
            nullptr, 0, [](LPVOID) -> DWORD { combat_test_loop(); return 0; }, nullptr, 0, nullptr));
    }
    // Templates and injection both live in the sniffer's hooks.
    if ((init->coop || init->clone_hotkeys) && !sniffer_ok) {
        log::warn("Co-op and the mirror need the sniffer ([sniffer] enabled=1); not active");
    } else if (init->coop) {
        // Combat values learnt in earlier sessions, and kept from now on
        // (cdmp_learnt.txt; learnt also while playing alone).
        net::learnt_load(init->dir);
        CloseHandle(CreateThread(
            nullptr, 0,
            [](LPVOID p) -> DWORD {
                std::unique_ptr<std::wstring> dir(static_cast<std::wstring*>(p));
                net::learnt_save_loop(*dir, g_stop);
                return 0;
            },
            new std::wstring(init->dir), 0, nullptr));
        // The co-op link sets the puppet offset (it depends on who is connected).
        net::puppet_set_horse_enabled(init->coop_horse);
        net::puppet_set_equip_enabled(init->coop_equip);
        // The mirrored damage and the other player's hits on our enemies'
        // copies would both hurt the same enemy: hits win.
        if (init->coop_damage && init->guest_hits) log::info("Co-op: mirrored damage off ([coop] guest_hits=1)");
        net::capture_set_damage_enabled(init->coop_damage && !init->guest_hits);
        net::puppet_install_climb_fix();
        net::puppet_install_rope_bend();
        // Horse and enemy copies (the other player may send theirs at any
        // time) need the crash containment (docs/INFORME-CABALLO-CRASH.md)
        // and the action-layer guards; without them they are refused.
        const bool contained = net::horse_diag_install(/*contain=*/true);
        const bool guarded = contained && net::puppet_install_layer_guards();
        if (!contained) {
            net::puppet_set_horse_enabled(false);
            log::warn("Horse and enemy copies disabled: their crash containment could not be installed");
        } else if (!guarded) {
            log::warn("Enemy copies disabled: their crash guards could not be installed");
        }
        net::enemy_set_receive_enabled(guarded);
        // Sending OUR enemies (Ctrl+F6 / Ctrl+F7): the host only.
        if (init->enemy_relay && guarded) {
            CloseHandle(CreateThread(
                nullptr, 0, [](LPVOID) -> DWORD { net::enemy_test_loop(g_stop); return 0; }, nullptr, 0, nullptr));
        }
        net::capture_install_equip_hook();
        if (init->attack_probe) {
            CloseHandle(CreateThread(
                nullptr, 0, [](LPVOID) -> DWORD { net::attack_probe_loop(g_stop); return 0; }, nullptr, 0, nullptr));
        }
        if (init->piece4_test) {
            CloseHandle(CreateThread(
                nullptr, 0, [](LPVOID) -> DWORD { net::piece4_test_loop(g_stop); return 0; }, nullptr, 0, nullptr));
        }
        net::guest_hits_set_enabled(init->guest_hits, init->guest_hit_stagger);
        net::hits_set_react(init->player_hit_react, init->player_hit_lethal);
        net::puppet_map_pin_style(init->pin_shape, init->pin_colour);
        // Before any puppet loads: the game picks its icon when it spawns.
        const bool minimap = net::puppet_minimap_install(init->minimap_icon);
        if ((init->aggro_test || init->enemies_target_copy) && net::aggro_test_install()) {
            net::aggro_set_copy_targetable(init->enemies_target_copy);
            net::aggro_set_copy_attack_slots(init->enemies_target_copy ? init->copy_attack_slots : 0);
            net::hits_set_enabled(init->enemies_target_copy);
            if (init->aggro_test) {
                CloseHandle(CreateThread(
                    nullptr, 0, [](LPVOID) -> DWORD { net::aggro_test_loop(g_stop); return 0; }, nullptr, 0,
                    nullptr));
            }
        }
        net::coop_set_ready();
        if (minimap) {
            CloseHandle(CreateThread(
                nullptr, 0,
                [](LPVOID cycle) -> DWORD {
                    if (cycle) {
                        minimap_cycle_loop();
                    } else {
                        while (!stop_requested(1000)) net::puppet_minimap_tick();
                    }
                    return 0;
                },
                init->minimap_cycle ? reinterpret_cast<LPVOID>(1) : nullptr, 0, nullptr));
        }
        if (init->clone_hotkeys) log::info("Mirror hotkeys off while co-op is enabled");
    } else if (init->clone_hotkeys) {
        net::puppet_set_offset(init->mirror_offset_m);
        net::puppet_install_climb_fix();
        net::capture_install_equip_hook();
        mirror_hotkey_loop(init->mirror_delay_ms);
    }
    return 0;
}

DWORD WINAPI worker(LPVOID) {
    const std::wstring dir = dir_of(module_path(g_self));
    log::open(dir + L"cdmp.log");
    log::info("=== Crimson Desert Multiplayer {} (build {:08X}{}, protocol {}) ===", CDMP_VERSION, kModBuild,
              CDMP_BUILD_DIRTY ? " + local changes" : "", proto::kVersion);
    log::info("Mod file: {}", narrow(module_path(g_self)));

    try {
        const bool in_game = log_process_info();
        const Config cfg = load_config(dir + L"cdmp.ini");
        log::info("Config: player '{}', peer {}:{}, {} Hz, net {}, co-op {}", cfg.player_name,
                  cfg.peer_host, cfg.peer_port, cfg.send_rate_hz, cfg.net_enabled ? "on" : "off",
                  cfg.coop_enabled ? "on" : "off");
        // Everything that changes behaviour, so a log sent by the other player
        // says how their game was set up.
        log::info("Config: net {} {} | coop delay {} ms, offset {:.2f} m, horse {}, equip {}, damage mirror {}, "
                  "map pin {}, minimap icon {:#x}, enemies target copy {}, guest hits {}, stagger {}, player react {} lethal {}",
                  cfg.net_transport_steam ? "steam" : "udp", cfg.net_role_host ? "host" : "guest", cfg.coop_delay_ms,
                  cfg.coop_offset_m, cfg.coop_horse, cfg.coop_equip, cfg.coop_damage, cfg.coop_map_pin,
                  cfg.coop_minimap_icon, cfg.coop_enemies_target_copy, cfg.coop_guest_hits, cfg.coop_guest_hit_stagger,
                  cfg.coop_player_hit_react, cfg.coop_player_hit_lethal);
        log::info("Config: dev clone {} combat {} minimap cycle {} aggro {} enemy relay {} piece4 {} | sniffer {} "
                  "record {} | overlay {}",
                  cfg.dev_clone_hotkeys, cfg.dev_combat_tests, cfg.dev_minimap_cycle, cfg.dev_aggro_test,
                  cfg.dev_enemy_relay, cfg.dev_piece4_test, cfg.sniffer_enabled, cfg.sniffer_record,
                  cfg.overlay_enabled);
        log::info("Config: time sync {}", cfg.coop_time_sync == 1   ? "send ours"
                                          : cfg.coop_time_sync == 2 ? "take the other player's"
                                                                    : "off");
        log::info("Config: enemy zone (guest) {} | {:.0f} m on, {:.0f} m off | restore {} state {} | fight wait {} s | "
                  "node events {} | lab {} ({:.0f} m)",
                  cfg.coop_suppress == 2 ? "ON" : cfg.coop_suppress == 1 ? "dry run" : "off",
                  cfg.coop_suppress_radius_m, cfg.coop_suppress_off_m, cfg.coop_suppress_restore,
                  cfg.coop_suppress_restore_state, cfg.coop_suppress_wait_fight_s, cfg.coop_suppress_node_events,
                  cfg.coop_suppress_lab, cfg.coop_suppress_lab_radius_m);
        if (in_game && cfg.coop_enabled) {
            net::enemy_set_auto(cfg.coop_enemy_auto && !cfg.coop_suppress_lab);
            net::guest_hits_set_finish_arrow(cfg.coop_finish_arrow);
            net::puppet_set_rope_lookup(cfg.dev_rope_piece_lookup);
            net::loot_set_enabled(cfg.coop_guest_loot && !cfg.net_role_host);
            net::puppet_minimap_set_enemy_offset(cfg.coop_enemy_minimap_icon);
            net::puppet_want_rope_bend(cfg.dev_rope_bend);
            net::time_sync_set_mode(static_cast<net::TimeSyncMode>(cfg.coop_time_sync));
            net::time_sync_set_dev_key(cfg.dev_time_sync_key);
            // Phase 5: only the guest hides its own enemies (the host relays them).
            if (!cfg.net_role_host && cfg.coop_suppress != 0) {
                net::SuppressSettings s;
                s.mode = cfg.coop_suppress == 2 ? net::SuppressMode::On : net::SuppressMode::DryRun;
                s.radius_m = cfg.coop_suppress_radius_m;
                s.off_m = cfg.coop_suppress_off_m;
                s.restore = cfg.coop_suppress_restore;
                s.restore_state = cfg.coop_suppress_restore_state;
                s.wait_fight_s = cfg.coop_suppress_wait_fight_s;
                s.respawn_grace_s = cfg.coop_suppress_respawn_grace_s;
                s.node_events = cfg.coop_suppress_node_events;
                s.lab = cfg.coop_suppress_lab;
                s.lab_radius_m = cfg.coop_suppress_lab_radius_m;
                s.english = cfg.overlay_english;
                net::suppress_configure(s);
                if (s.lab) log::info("Suppress lab: automatic enemy copies off (they would be hidden too)");
            }
        }

        if (cfg.startup_delay_ms > 0 && stop_requested(cfg.startup_delay_ms)) return 0;

        LocalSource local(in_game);

        // The overlay only makes sense inside the game; a test host would
        // otherwise keep patched DXGI vtables pointing into an unloaded DLL.
        // cdmp_dx12_test opts in explicitly (and never unloads the mod).
        const bool overlay_allowed =
            in_game || GetEnvironmentVariableW(L"CDMP_TEST_OVERLAY", nullptr, 0) > 0;
        const bool overlay = overlay_allowed && cfg.overlay_enabled;
        const bool sniffer = in_game && cfg.sniffer_enabled;
        if (in_game && !cfg.overlay_enabled) log::info("Overlay disabled in cdmp.ini");
        if (in_game && !cfg.sniffer_enabled) log::info("Sniffer disabled in cdmp.ini");
        if (overlay || sniffer) {
            if (overlay) render::hud_configure({cfg.overlay_self_marker, cfg.overlay_english});
            g_late_thread = CreateThread(nullptr, 0, late_init,
                                         new LateInit{in_game, overlay, sniffer,
                                                      in_game && cfg.dev_clone_hotkeys,
                                                      cfg.dev_mirror_delay_ms, cfg.dev_mirror_offset_m,
                                                      in_game && cfg.coop_enabled, cfg.coop_offset_m,
                                                      cfg.coop_horse, cfg.coop_equip, cfg.coop_damage,
                                                      in_game && cfg.dev_combat_tests, cfg.sniffer_record,
                                                      dir, cfg.coop_minimap_icon, cfg.dev_minimap_cycle,
                                                      in_game && cfg.dev_aggro_test,
                                                      cfg.coop_enemies_target_copy,
                                                      in_game && cfg.dev_enemy_relay,
                                                      in_game && cfg.dev_piece4_test,
                                                      cfg.coop_guest_hits, cfg.coop_guest_hit_stagger,
                                                      cfg.coop_player_hit_react, cfg.coop_player_hit_lethal,
                                                      in_game && cfg.dev_attack_probe, cfg.coop_copy_attack_slots,
                                                      cfg.coop_map_pin_shape, cfg.coop_map_pin_colour},
                                         0, nullptr);
        }

        if (cfg.net_enabled) run_network(cfg, local);
    } catch (const std::exception& e) {
        log::error("Unhandled exception in worker: {}", e.what());
    } catch (...) {
        log::error("Unhandled non-standard exception in worker");
    }

    log::info("Worker exiting");
    return 0;
}

}  // namespace

void start(HMODULE self) {
    g_self = self;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
}

void stop() {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 10000);
    CloseHandle(g_thread);
    if (g_late_thread) {
        WaitForSingleObject(g_late_thread, 10000);
        CloseHandle(g_late_thread);
        g_late_thread = nullptr;
    }
    CloseHandle(g_stop);
    g_thread = nullptr;
    g_stop = nullptr;
    log::close();
}

}  // namespace cdmp
