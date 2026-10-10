#include "mod/config.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace cdmp {
namespace {

constexpr wchar_t kDefaultIni[] =
    L"; Crimson Desert Multiplayer (cdmp) settings\r\n"
    L"[general]\r\n"
    L"player_name=Player\r\n"
    L"; Wait before starting, in milliseconds\r\n"
    L"startup_delay_ms=0\r\n"
    L"\r\n"
    L"[net]\r\n"
    L"enabled=1\r\n"
    L"; steam = play with a Steam friend; udp = tests on this PC only (cdmp_peer.exe)\r\n"
    L"transport=udp\r\n"
    L"; guest connects to the host or to cdmp_peer; host waits for one player\r\n"
    L"role=guest\r\n"
    L"; UDP: address of the host or of cdmp_peer.exe (a UDP host listens on peer_port)\r\n"
    L"peer_host=127.0.0.1\r\n"
    L"peer_port=37015\r\n"
    L"send_rate_hz=20\r\n"
    L"\r\n"
    L"[steam]\r\n"
    L"; Guest: the host's SteamID64 (empty = the only friend playing Crimson Desert)\r\n"
    L"; Host: accept only this friend (empty = any friend playing)\r\n"
    L"peer_steamid=\r\n"
    L"\r\n"
    L"[overlay]\r\n"
    L"; 0 turns off the in-game overlay completely (no graphics hooks)\r\n"
    L"enabled=1\r\n"
    L"; Draw a cross on your own character (to check the projection)\r\n"
    L"self_marker=1\r\n"
    L"; Language of the panel's texts: en | es\r\n"
    L"language=en\r\n"
    L"\r\n"
    L"[sniffer]\r\n"
    L"; Needed for co-op (reads and sends the game's internal messages)\r\n"
    L"enabled=1\r\n"
    L"; 1 also records them to cdmp_packets_* files (50-100 MB per session), to report a problem\r\n"
    L"record=0\r\n"
    L"\r\n"
    L"[coop]\r\n"
    L"; 1 shows the other player (through cdmp_peer) as a character in your world\r\n"
    L"enabled=0\r\n"
    L"; Playout delay in ms (longer = smoother climbing, more lag)\r\n"
    L"delay_ms=300\r\n"
    L"; Sideways offset of the other player in metres (0 = exact position)\r\n"
    L"offset_m=0\r\n";

std::wstring read_string(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                         const std::string& fallback) {
    wchar_t buf[256];
    const std::wstring def(fallback.begin(), fallback.end());
    GetPrivateProfileStringW(section, key, def.c_str(), buf, 256, path.c_str());
    return buf;
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

uint32_t read_uint(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                   uint32_t fallback) {
    return GetPrivateProfileIntW(section, key, static_cast<INT>(fallback), path.c_str());
}

// A distance in metres, within +-50; anything else keeps the fallback.
float read_metres(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                  float fallback) {
    const std::wstring s = read_string(path, section, key, "");
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    const float v = std::wcstof(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(v) && std::fabs(v) <= 50.0f ? v : fallback;
}

// A number within [lo, hi]; anything else keeps the fallback.
float read_float(const std::wstring& path, const wchar_t* section, const wchar_t* key, float fallback, float lo,
                 float hi) {
    const std::wstring s = read_string(path, section, key, "");
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    const float v = std::wcstof(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(v) && v >= lo && v <= hi ? v : fallback;
}

}  // namespace

Config load_config(const std::wstring& ini_path) {
    if (GetFileAttributesW(ini_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(ini_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // UTF-16 BOM so GetPrivateProfileStringW reads it as Unicode.
            const wchar_t bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(h, &bom, sizeof bom, &written, nullptr);
            WriteFile(h, kDefaultIni, sizeof kDefaultIni - sizeof(wchar_t), &written, nullptr);
            CloseHandle(h);
        }
    }

    Config c;
    c.player_name = narrow(read_string(ini_path, L"general", L"player_name", c.player_name));
    c.startup_delay_ms = read_uint(ini_path, L"general", L"startup_delay_ms", c.startup_delay_ms);
    c.net_enabled = read_uint(ini_path, L"net", L"enabled", 1) != 0;
    c.peer_host = narrow(read_string(ini_path, L"net", L"peer_host", c.peer_host));
    c.peer_port = static_cast<uint16_t>(read_uint(ini_path, L"net", L"peer_port", c.peer_port));
    c.send_rate_hz = std::clamp(read_uint(ini_path, L"net", L"send_rate_hz", c.send_rate_hz), 1u, 60u);
    c.net_transport_steam = read_string(ini_path, L"net", L"transport", "udp") == L"steam";
    c.net_role_host = read_string(ini_path, L"net", L"role", "guest") == L"host";
    c.steam_peer_id = _wcstoui64(read_string(ini_path, L"steam", L"peer_steamid", "").c_str(), nullptr, 10);
    c.steam_channel = static_cast<int>(read_uint(ini_path, L"steam", L"channel", 67));
    c.overlay_enabled = read_uint(ini_path, L"overlay", L"enabled", 1) != 0;
    c.overlay_self_marker = read_uint(ini_path, L"overlay", L"self_marker", 1) != 0;
    c.overlay_english = read_string(ini_path, L"overlay", L"language", "en") != L"es";
    c.sniffer_enabled = read_uint(ini_path, L"sniffer", L"enabled", 1) != 0;
    c.sniffer_record = read_uint(ini_path, L"sniffer", L"record", 0) != 0;  // only when asked for
    c.dev_clone_hotkeys = read_uint(ini_path, L"dev", L"clone_hotkeys", 0) != 0;
    c.dev_mirror_delay_ms = std::min(read_uint(ini_path, L"dev", L"mirror_delay_ms", 300), 5000u);
    c.dev_mirror_offset_m = read_metres(ini_path, L"dev", L"mirror_offset_m", c.dev_mirror_offset_m);
    c.dev_combat_tests = read_uint(ini_path, L"dev", L"combat_tests", 0) != 0;
    c.dev_minimap_cycle = read_uint(ini_path, L"dev", L"minimap_cycle", 0) != 0;
    c.dev_aggro_test = read_uint(ini_path, L"dev", L"aggro_test", 0) != 0;
    c.dev_enemy_relay = read_uint(ini_path, L"dev", L"enemy_relay", 0) != 0;
    c.dev_piece4_test = read_uint(ini_path, L"dev", L"piece4_test", 0) != 0;
    c.dev_attack_probe = read_uint(ini_path, L"dev", L"attack_probe", 0) != 0;
    c.coop_enabled = read_uint(ini_path, L"coop", L"enabled", 0) != 0;
    c.coop_delay_ms = std::clamp(read_uint(ini_path, L"coop", L"delay_ms", c.coop_delay_ms), 50u, 5000u);
    c.coop_offset_m = read_metres(ini_path, L"coop", L"offset_m", c.coop_offset_m);
    c.coop_horse = read_uint(ini_path, L"coop", L"horse", 0) != 0;
    c.coop_equip = read_uint(ini_path, L"coop", L"equip", 1) != 0;
    c.coop_damage = read_uint(ini_path, L"coop", L"damage", 0) != 0;
    c.coop_map_pin = read_uint(ini_path, L"coop", L"map_pin", 1) != 0;
    c.coop_enemies_target_copy = read_uint(ini_path, L"coop", L"enemies_target_copy", 0) != 0;
    c.coop_guest_hits = read_uint(ini_path, L"coop", L"guest_hits", 0) != 0;
    c.coop_guest_hit_stagger = read_uint(ini_path, L"coop", L"guest_hit_stagger", 0) != 0;
    c.coop_player_hit_react = read_uint(ini_path, L"coop", L"player_hit_react", 0) != 0;
    c.coop_player_hit_lethal = read_uint(ini_path, L"coop", L"player_hit_lethal", 0) != 0;
    c.coop_time_sync = static_cast<int>(std::min(read_uint(ini_path, L"coop", L"time_sync", 0), 2u));
    c.dev_time_sync_key = read_uint(ini_path, L"dev", L"time_sync_key", 0) != 0;
    c.coop_enemy_auto = read_uint(ini_path, L"coop", L"enemy_auto", 1) != 0;
    c.coop_finish_arrow = read_uint(ini_path, L"coop", L"finish_arrow", 1) != 0;
    c.dev_rope_piece_lookup = read_uint(ini_path, L"dev", L"rope_piece_lookup", 0) != 0;
    c.coop_guest_loot = read_uint(ini_path, L"coop", L"guest_loot", 0) != 0;
    c.dev_rope_bend = read_uint(ini_path, L"dev", L"rope_bend", 0) != 0;
    {
        const std::wstring v = read_string(ini_path, L"coop", L"enemy_minimap_icon", "");
        if (!v.empty()) c.coop_enemy_minimap_icon = static_cast<int>(std::wcstol(v.c_str(), nullptr, 0));
    }
    c.coop_copy_attack_slots = static_cast<int>(std::min(read_uint(ini_path, L"coop", L"copy_attack_slots", 0), 8u));
    c.coop_map_pin_shape = static_cast<uint8_t>(std::min(read_uint(ini_path, L"coop", L"map_pin_shape", 4), 13u));
    c.coop_map_pin_colour = static_cast<uint8_t>(std::min(read_uint(ini_path, L"coop", L"map_pin_colour", 14), 14u));
    {
        // 0 / off, dry, 1 / on; anything else is the dry run.
        const std::wstring v = read_string(ini_path, L"coop", L"suppress", "dry");
        c.coop_suppress = (v == L"0" || v == L"off") ? 0 : (v == L"1" || v == L"on") ? 2 : 1;
        c.coop_suppress_radius_m = read_float(ini_path, L"coop", L"suppress_radius_m", 150.0f, 50.0f, 300.0f);
        c.coop_suppress_off_m = read_float(ini_path, L"coop", L"suppress_off_m", c.coop_suppress_radius_m + 100.0f,
                                           c.coop_suppress_radius_m + 5.0f, c.coop_suppress_radius_m + 100.0f);
        c.coop_suppress_restore = read_uint(ini_path, L"coop", L"suppress_restore", 1) != 0;
        c.coop_suppress_restore_state = read_uint(ini_path, L"coop", L"suppress_restore_state", 1) != 0;
        c.coop_suppress_wait_fight_s = std::min(read_uint(ini_path, L"coop", L"suppress_wait_fight_s", 60), 600u);
        c.coop_suppress_respawn_grace_s =
            std::min(read_uint(ini_path, L"coop", L"suppress_respawn_grace_s", 60), 180u);
        c.coop_suppress_node_events = read_uint(ini_path, L"coop", L"suppress_node_events", 0) != 0;
        c.coop_suppress_lab = read_uint(ini_path, L"coop", L"suppress_lab", 0) != 0;
        c.coop_suppress_lab_radius_m = read_float(ini_path, L"coop", L"suppress_lab_radius_m", 60.0f, 10.0f, 300.0f);
    }
    {
        // Decimal or 0x-hex; "-1" diagnostic.
        const std::wstring v = read_string(ini_path, L"coop", L"minimap_icon", "");
        if (!v.empty()) c.coop_minimap_icon = static_cast<int>(std::wcstol(v.c_str(), nullptr, 0));
    }
    return c;
}

}  // namespace cdmp
