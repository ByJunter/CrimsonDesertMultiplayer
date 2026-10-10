#pragma once
// Settings read from cdmp.ini next to the mod. Missing keys use the defaults
// below, and a default file is written if none exists.

#include <cstdint>
#include <string>

namespace cdmp {

struct Config {
    std::string player_name = "Player";
    uint32_t startup_delay_ms = 0;

    std::string peer_host = "127.0.0.1";
    uint16_t peer_port = 37015;
    uint32_t send_rate_hz = 20;
    bool net_enabled = true;
    // [net] transport=udp|steam, role=guest|host. A guest connects to the host
    // (or to cdmp_peer); a host waits for one player.
    bool net_transport_steam = false;
    bool net_role_host = false;
    // [steam] peer_steamid: guest = the host's SteamID64 (0 = the only friend
    // playing); host = accept only this friend (0 = any friend playing).
    uint64_t steam_peer_id = 0;
    int steam_channel = 67;

    bool overlay_enabled = true;  // kill switch for the DX12 hook
    bool overlay_self_marker = true;  // dev aid while validating the projection
    bool overlay_english = true;      // [overlay] language = en | es: the panel's texts

    bool sniffer_enabled = true;  // hook internal client/server packets (needed by co-op)
    bool sniffer_record = false;  // also write them to cdmp_packets_*.log/.bin (dev; 50-100 MB/session)

    // Experiment 4c: F9 spawns a clone of the player, F10 removes it. Writes
    // packets into the game, so it is off unless explicitly enabled.
    bool dev_clone_hotkeys = false;
    uint32_t dev_mirror_delay_ms = 300;  // simulated latency of the mirrored clone
    float dev_mirror_offset_m = 2.5f;    // sideways offset; 0 = walk the player's exact path
    // Combat injection tests (combate-coop.md §6, T2/T3): WRITE to the game's
    // server. Only with a save backup, and never save afterwards.
    bool dev_combat_tests = false;
    // Ctrl+F9 / Ctrl+Shift+F9: next / previous minimap icon for the other player.
    bool dev_minimap_cycle = false;
    // Ctrl+F5: an enemy targets the other player's copy (aggro_test.h).
    bool dev_aggro_test = false;
    // Ctrl+F6: send the nearest enemy to the other player (enemy_relay.h).
    bool dev_enemy_relay = false;
    // Ctrl+F10 / Ctrl+F11 experiments of piece 4 (piece4_test.h); they write to the game.
    bool dev_piece4_test = false;
    // Every 2 s, log who sits in the attacker lists of our player and of the
    // other player's copy (attack_probe.h). Read only.
    bool dev_attack_probe = false;

    // Co-op over the network ([coop]): the other player (or, through
    // cdmp_peer --mode relay with nobody else connected, yourself) appears as
    // a remote character. Writes packets into the game; off by default.
    bool coop_enabled = false;
    uint32_t coop_delay_ms = 300;  // playout delay; longer = smoother climb-ups
    float coop_offset_m = 0.0f;    // sideways offset of the remote character
    // Experimental, off by default: the copy's horse is not created by the
    // client (owned load) or crashes the game (ownerless load); live tests 1-2.
    bool coop_horse = false;       // the other player's horse + mount/dismount fixes
    // Works since the copy got its own character number (live test 3).
    bool coop_equip = true;        // relay equipment changes
    // Mirrored damage: each world keeps its enemies; hits by the other player
    // are applied to our enemy of the same type nearest to theirs. Off by
    // default: it lets the other side hurt our world's NPCs (security review
    // H1); guest_hits replaces it.
    bool coop_damage = false;
    // The other player as a pin on the world map and minimap.
    bool coop_map_pin = true;
    uint8_t coop_map_pin_shape = 4;    // 0-13 (the game's default pin: 4)
    uint8_t coop_map_pin_colour = 14;  // 0-14 (the game's default pin: 14)
    // The other player's icon on the minimap: offset in the game's icon
    // table (0x124 = an outlined arrow, the one chosen in the live test;
    // 0x126 = our own arrow; no other colours exist), 0 = none,
    // -1 = diagnostic (log what the game decides).
    int coop_minimap_icon = 0x124;
    // Our enemies may attack the other player's copy (aggro_test.h). Off
    // until the hits reach the other player and they see our enemies.
    bool coop_enemies_target_copy = false;
    // The other player's blows on the copies of our enemies hurt them
    // (guest_hits.h). Off until tested with two players.
    bool coop_guest_hits = false;
    // ...and stagger them (restriction + hit buff, pieza4-tambaleo.md):
    // experimental until a live test shows the stagger.
    bool coop_guest_hit_stagger = false;
    // Our player reacts (stagger, knock-down) to the other player's enemies'
    // blows on our copy over there: experimental until tested.
    bool coop_player_hit_react = false;
    // ...and can die from them (a normal death): experimental until tested.
    bool coop_player_hit_lethal = false;
    // How many of our enemies at a time may attack the other player's copy
    // (copia-objetivo-sin-ataques.md); 0 = the game decides (they mostly wait).
    int coop_copy_attack_slots = 0;
    // Time of day (hora-del-dia.md): 0 off, 1 send ours (host), 2 take the
    // other player's (guest).
    int coop_time_sync = 0;
    // Host: copies of our enemies that fight go to the guest on their own
    // (Ctrl+F7 turns them off and on). Needs [dev] enemy_relay=1.
    bool coop_enemy_auto = true;
    // Host: the guest's lethal blows end with an arrow (normal death) instead of a collision.
    bool coop_finish_arrow = true;
    // The other player's tree hook names our own piece of that tree (calls a game function).
    bool dev_rope_piece_lookup = false;
    // Guest: loot from the host's enemies we help kill, via a twin on our server (loot_twin.h).
    bool coop_guest_loot = false;
    // Minimap icon (table offset) of the other player's enemy copies; 0 = the game's (none).
    // +0x10A Actor_Enemy (the red dot); +0x112 is the boss diamond (minimapa-icono-enemigo.md).
    int coop_enemy_minimap_icon = 0x10A;
    // The tree the other player's copy pulls bends here too (calls game code).
    bool dev_rope_bend = false;
    bool dev_time_sync_key = false;  // Ctrl+Shift+F12: our time 3 h forward
    // Phase 5 (native_suppress.h), guest: within suppress_radius_m of the
    // host our own hostile enemies are hidden and the host's copies stand in
    // for them; beyond suppress_off_m they come back. [coop] suppress = 0 (off),
    // dry (only logs what it would hide; the default until tested) or 1.
    int coop_suppress = 1;  // 0 off, 1 dry run, 2 on
    float coop_suppress_radius_m = 150.0f;
    float coop_suppress_off_m = 250.0f;
    bool coop_suppress_restore = true;        // our natives come back when the zone ends
    bool coop_suppress_restore_state = true;  // with what the server said about them meanwhile
    uint32_t coop_suppress_wait_fight_s = 60;  // wait this long at most for a fight with them to end
    uint32_t coop_suppress_respawn_grace_s = 60;  // the host died and reappeared: keep the zone this long
    bool coop_suppress_node_events = false;   // node and faction events too
    // Lab (one PC, no partner): Ctrl+Home turns a zone around you on and off;
    // the relay of our enemies is off then.
    bool coop_suppress_lab = false;
    float coop_suppress_lab_radius_m = 60.0f;
};

Config load_config(const std::wstring& ini_path);

}  // namespace cdmp
