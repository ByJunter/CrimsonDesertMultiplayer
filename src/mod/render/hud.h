#pragma once
// What the overlay shows: a status panel and a marker over the other player.
// The network thread publishes data with hud_update(); the render hook calls
// hud_draw() once per frame on the game's present thread.

#include <optional>
#include <string>

#include "common/protocol.h"
#include "mod/game/player_reader.h"

namespace cdmp::render {

struct HudData {
    bool connected = false;
    std::string peer_name;
    float rtt_ms = 0.0f;
    std::optional<game::PlayerSnapshot> local;
    std::string local_status;
    std::optional<proto::PlayerState> remote;
    std::string peer_problem;  // another protocol version or mod build ("" if none)
    std::string enemy_zone;    // phase 5: whose enemies the guest sees ("" = nothing to say)
};

struct HudOptions {
    bool self_marker = false;  // dev aid: draw a cross on our own character
    bool english = true;       // the panel's texts in English (else Spanish)
};

void hud_configure(const HudOptions& options);
void hud_update(HudData data);
void hud_draw(float width, float height);

}  // namespace cdmp::render
