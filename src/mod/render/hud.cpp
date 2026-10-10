#include "mod/render/hud.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <mutex>

#include "common/math3d.h"
#include "mod/game/camera_reader.h"
#include "mod/log.h"
#include "mod/render/dx12_hook.h"

namespace cdmp::render {
namespace {

constexpr float kHeadHeight = 2.1f;  // marker above the character's feet (m)
constexpr ImU32 kAccent = IM_COL32(230, 70, 60, 255);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);

std::mutex g_mutex;
HudData g_data;
HudOptions g_options;

void text_shadowed(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text) {
    dl->AddText(ImVec2(pos.x + 1, pos.y + 1), kShadow, text);
    dl->AddText(pos, color, text);
}

void draw_bar(ImDrawList* dl, ImVec2 center, float width, float fraction, ImU32 color) {
    const ImVec2 a(center.x - width / 2, center.y);
    const ImVec2 b(center.x + width / 2, center.y + 5);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 160));
    dl->AddRectFilled(a, ImVec2(a.x + width * std::clamp(fraction, 0.0f, 1.0f), b.y), color);
}

void draw_remote_marker(ImDrawList* dl, const game::ScreenPoint& p, const HudData& d,
                        float distance) {
    const ImVec2 c(p.x, p.y);
    dl->AddCircleFilled(c, 7.0f, kShadow);
    dl->AddCircleFilled(c, 5.0f, kAccent);
    const std::string name = d.peer_name.empty() ? (g_options.english ? "Player 2" : "Jugador 2") : d.peer_name;
    const ImVec2 size = ImGui::CalcTextSize(name.c_str());
    text_shadowed(dl, ImVec2(c.x - size.x / 2, c.y - 28), IM_COL32_WHITE, name.c_str());
    if (d.remote->health_max > 0) {
        draw_bar(dl, ImVec2(c.x, c.y - 12), 70, d.remote->health / d.remote->health_max,
                 IM_COL32(90, 200, 90, 255));
    }
    char dist[32];
    std::snprintf(dist, sizeof dist, "%.0f m", distance);
    const ImVec2 ds = ImGui::CalcTextSize(dist);
    text_shadowed(dl, ImVec2(c.x - ds.x / 2, c.y + 10), IM_COL32(220, 220, 220, 255), dist);
}

}  // namespace

void hud_configure(const HudOptions& options) {
    std::lock_guard lock(g_mutex);
    g_options = options;
}

void hud_update(HudData data) {
    std::lock_guard lock(g_mutex);
    g_data = std::move(data);
}

void hud_draw(float width, float height) {
    static bool visible = true;
    static bool f8_was_down = false;
    static game::CameraReader camera;
    static bool camera_init = false;
    static ULONGLONG last_diag = 0;

    const bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8_down && !f8_was_down) visible = !visible;
    f8_was_down = f8_down;
    if (!visible) return;

    HudData d;
    HudOptions opt;
    {
        std::lock_guard lock(g_mutex);
        d = g_data;
        opt = g_options;
    }
    if (!camera_init) {
        camera.init();
        camera_init = true;
    }
    const auto cam = camera.read();

    // Is the camera position stored in world space or relative to the map
    // chunk like the player's physics position? Whichever is closer to us.
    std::optional<proto::Vec3> cam_world;
    bool cam_is_local = false;
    if (cam && d.local) {
        const float to_world = math::length(math::sub(cam->position, d.local->position));
        const float to_local = math::length(math::sub(cam->position, d.local->local_position));
        cam_is_local = to_local < to_world;
        cam_world = cam_is_local ? math::add(cam->position, d.local->chunk_origin) : cam->position;
    }

    // ---- status panel
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("cdmp", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.25f, 1), "Crimson Desert Multiplayer %s", CDMP_VERSION);
    ImGui::SameLine();
    ImGui::TextDisabled("[F8]");
    const bool en = opt.english;
    if (d.connected) {
        ImGui::Text(en ? "Network: connected to '%s' (%.1f ms)" : "Red: conectado a '%s' (%.1f ms)",
                    d.peer_name.c_str(), d.rtt_ms);
    } else {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                           en ? "Network: looking for the other player..." : "Red: buscando al otro jugador...");
    }
    if (!d.peer_problem.empty()) {
        // In English from the session; the essential part is the instruction.
        ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), en ? "Different mod versions: both of you update"
                                                          : "Versiones distintas del mod: actualizad los dos");
        ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), "%s", d.peer_problem.c_str());
    }
    if (d.local) {
        ImGui::Text(en ? "You: HP %.0f/%.0f  Stamina %.0f/%.0f" : "Tú: HP %.0f/%.0f  Stamina %.0f/%.0f",
                    d.local->health, d.local->health_max, d.local->stamina, d.local->stamina_max);
    } else {
        ImGui::TextDisabled(en ? "You: %s" : "Tú: %s", d.local_status.c_str());
    }
    float remote_distance = 0;
    if (d.remote && d.local) {
        remote_distance = math::length(math::sub(d.remote->pos, d.local->position));
        ImGui::Text(en ? "Other player: HP %.0f/%.0f  %.0f m away" : "Otro jugador: HP %.0f/%.0f  a %.0f m",
                    d.remote->health, d.remote->health_max, remote_distance);
    }
    if (!d.enemy_zone.empty()) ImGui::TextColored(ImVec4(0.75f, 0.85f, 1, 1), "%s", d.enemy_zone.c_str());
    if (!cam) ImGui::TextDisabled(en ? "Camera: %s" : "Cámara: %s", camera.last_failure().c_str());
    ImGui::End();

    // ---- world markers
    if (!cam || !cam_world) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (d.remote) {
        const proto::Vec3 head{d.remote->pos.x, d.remote->pos.y + kHeadHeight, d.remote->pos.z};
        if (auto p = game::project(*cam, *cam_world, head, width, height)) {
            draw_remote_marker(dl, *p, d, remote_distance);
        }
    }

    std::optional<game::ScreenPoint> self;
    if (d.local) {
        const proto::Vec3 chest{d.local->position.x, d.local->position.y + 1.2f,
                                d.local->position.z};
        self = game::project(*cam, *cam_world, chest, width, height);
        if (opt.self_marker && self) {
            const ImVec2 c(self->x, self->y);
            dl->AddLine(ImVec2(c.x - 8, c.y), ImVec2(c.x + 8, c.y), IM_COL32(80, 220, 255, 255), 2);
            dl->AddLine(ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 8), IM_COL32(80, 220, 255, 255), 2);
        }
    }

    // Numbers that let us verify the projection from the log alone: in third
    // person our own chest should land near the horizontal centre.
    const ULONGLONG now = GetTickCount64();
    if (now - last_diag >= 5000 && d.local) {
        last_diag = now;
        const auto st = status();
        log::info("Overlay diag: frames {} | cam {} space, axes {} | cam->player {:.2f} m | "
                  "self on screen {} | fov {:.1f} deg",
                  st.frames_drawn, cam_is_local ? "chunk-local" : "world",
                  cam->axes_from_view ? "from view matrix" : "fallback (cross)",
                  math::length(math::sub(*cam_world, d.local->position)),
                  self ? std::format("({:.0f}, {:.0f}) of {:.0f}x{:.0f}, depth {:.2f}", self->x,
                                     self->y, width, height, self->depth)
                       : std::string("behind camera"),
                  2.0f * std::atan(1.0f / cam->p11) * 57.2958f);
    }
}

}  // namespace cdmp::render
