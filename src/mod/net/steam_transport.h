#pragma once
// Datagrams over Steam (ISteamNetworkingMessages): two friends connect with no
// port forwarding, through Valve's relays when needed. Uses the game's own,
// already initialised steam_api64.dll through its flat C exports. Never calls
// SteamAPI_RunCallbacks or ManualDispatch (that would break the game's own
// callbacks) and registers no callbacks: incoming sessions are accepted by
// polling AcceptSessionWithUser for friends who are playing Crimson Desert.
// Design and sources: docs/research/steam-transporte.md.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/transport.h"

namespace cdmp::net {

struct SteamFriend {
    uint64_t id;
    std::string name;
};

class SteamTransport final : public Transport {
public:
    // accept_from: 0 = accept any immediate friend playing the game (host),
    // otherwise only that SteamID64.
    SteamTransport(int channel, bool host, uint64_t accept_from);
    ~SteamTransport() override;

    bool send(const PeerAddr& to, std::span<const uint8_t> data) override;
    std::optional<size_t> recv(PeerAddr& from, std::span<uint8_t> buf, int timeout_ms) override;
    void poll(uint32_t now_ms) override;
    bool ready() const override { return ready_; }
    std::string status() const override;

    // Immediate friends currently in Crimson Desert (empty until ready).
    std::vector<SteamFriend> friends_playing();
    uint64_t own_id() const { return own_id_; }
    // Whom we talk to (for connection status in the log).
    void set_peer(uint64_t id) { peer_ = id; }

private:
    struct Api;
    bool resolve();
    void become_ready();
    void fail(const char* what);
    void drain_incoming();

    std::unique_ptr<Api> api_;
    int channel_;
    bool host_;
    uint64_t accept_from_;
    bool disabled_ = false;
    bool ready_ = false;
    uint64_t own_id_ = 0;
    uint64_t peer_ = 0;
    uint32_t last_poll_ms_ = 0;
    uint32_t started_ms_ = 0;
    int last_session_state_ = -1;
    int last_relay_status_ = -999;
    std::string last_status_ = "-";  // ping, throughput, queue of the session
    uint64_t sent_ = 0, received_ = 0, send_errors_ = 0;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> inbox_;
};

}  // namespace cdmp::net
