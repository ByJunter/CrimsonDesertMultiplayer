#include "mod/net/steam_transport.h"

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <format>

#include "common/clock.h"
#include "common/protocol.h"
#include "mod/log.h"

namespace cdmp::net {
namespace {

// ---------------------------------------------------------------- flat API
// Our own declarations: the Steamworks SDK is not vendored. Layouts from the
// public SDK (VALVE_CALLBACK_PACK_LARGE = pack 8 on Windows), checked below.
#pragma pack(push, 8)
struct SteamNetworkingIdentity {  // 136 bytes
    int32_t type;                 // ESteamNetworkingIdentityType: 16 = SteamID
    int32_t size;                 // 8 for a SteamID
    union {
        uint64_t steam_id64;
        uint32_t reserved[32];
    };
};
static_assert(sizeof(SteamNetworkingIdentity) == 136);

struct SteamNetworkingMessage {  // SteamNetworkingMessage_t, 216 bytes
    void* data;
    int32_t size;
    uint32_t conn;
    SteamNetworkingIdentity peer;
    int64_t conn_user_data;
    int64_t usec_received;
    int64_t message_number;
    void (*free_data)(SteamNetworkingMessage*);
    void (*release)(SteamNetworkingMessage*);
    int32_t channel;
    int32_t flags;
    int64_t user_data;
    uint16_t lane;
    uint16_t pad;
};
static_assert(offsetof(SteamNetworkingMessage, peer) == 16);
static_assert(offsetof(SteamNetworkingMessage, channel) == 192);
static_assert(sizeof(SteamNetworkingMessage) == 216);

// SteamNetConnectionRealTimeStatus_t (SDK isteamnetworkingtypes.h), 120 bytes.
struct RealTimeStatus {
    int32_t state;
    int32_t ping_ms;
    float quality_local, quality_remote;
    float out_packets_per_sec, out_bytes_per_sec;
    float in_packets_per_sec, in_bytes_per_sec;
    int32_t send_rate_bytes_per_sec;
    int32_t pending_unreliable, pending_reliable, sent_unacked_reliable;
    int64_t queue_time_usec;
    uint32_t reserved[16];
};
static_assert(offsetof(RealTimeStatus, queue_time_usec) == 48);
static_assert(sizeof(RealTimeStatus) == 120);

struct FriendGameInfo {  // FriendGameInfo_t, 24 bytes
    uint64_t game_id;    // CGameID; a plain app = its AppID in the low 24 bits
    uint32_t game_ip;
    uint16_t game_port, query_port;
    uint64_t steam_id_lobby;
};
static_assert(sizeof(FriendGameInfo) == 24);
#pragma pack(pop)

constexpr int32_t kIdentitySteamID = 16;
constexpr int kSendUnreliableNoNagle = 1;
constexpr int kSendAutoRestartBrokenSession = 32;
constexpr int kFriendFlagImmediate = 0x04;
constexpr int kResultOK = 1;
constexpr uint32_t kAppId = 3321460;  // Crimson Desert
constexpr uint32_t kPollEveryMs = 500;
constexpr uint32_t kReadyTimeoutMs = 120000;

SteamNetworkingIdentity identity(uint64_t id) {
    SteamNetworkingIdentity i;
    std::memset(&i, 0, sizeof i);
    i.type = kIdentitySteamID;
    i.size = 8;
    i.steam_id64 = id;
    return i;
}

// Every call into Steam goes through one of these: a crash in there would be
// reported to Pearl Abyss as a game crash, so the first exception disables
// the transport instead. Templates with only trivially copyable arguments,
// so __try is allowed.
template <class R, class... P, class... A>
bool call(R& out, R (*fn)(P...), A... args) {
    if (!fn) return false;
    __try {
        out = fn(args...);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class... P, class... A>
bool call_void(void (*fn)(P...), A... args) {
    if (!fn) return false;
    __try {
        fn(args...);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

struct SteamTransport::Api {
    // Accessors and globals.
    int32_t (*get_hsteam_user)() = nullptr;
    void (*release_thread_memory)() = nullptr;
    void* (*messages)() = nullptr;
    void* (*utils)() = nullptr;
    void* (*user)() = nullptr;
    void* (*friends)() = nullptr;
    // ISteamNetworkingMessages
    int32_t (*send_to_user)(void*, const SteamNetworkingIdentity*, const void*, uint32_t, int, int) = nullptr;
    int (*receive)(void*, int, SteamNetworkingMessage**, int) = nullptr;
    bool (*accept)(void*, const SteamNetworkingIdentity*) = nullptr;
    bool (*close)(void*, const SteamNetworkingIdentity*) = nullptr;
    int (*session_info)(void*, const SteamNetworkingIdentity*, void*, void*) = nullptr;
    void (*message_release)(SteamNetworkingMessage*) = nullptr;
    // ISteamNetworkingUtils
    void (*init_relay)(void*) = nullptr;
    bool (*set_global_int)(void*, int, int32_t) = nullptr;
    int (*relay_status)(void*, void*) = nullptr;
    // ISteamUser
    uint64_t (*get_steam_id)(void*) = nullptr;
    bool (*logged_on)(void*) = nullptr;
    // ISteamFriends
    const char* (*persona_name)(void*) = nullptr;
    int (*friend_count)(void*, int) = nullptr;
    uint64_t (*friend_by_index)(void*, int, int) = nullptr;
    const char* (*friend_name)(void*, uint64_t) = nullptr;
    bool (*friend_game)(void*, uint64_t, FriendGameInfo*) = nullptr;

    void* msgs = nullptr;
    void* utl = nullptr;
    void* usr = nullptr;
    void* frn = nullptr;
};

SteamTransport::SteamTransport(int channel, bool host, uint64_t accept_from)
    : api_(std::make_unique<Api>()), channel_(channel), host_(host), accept_from_(accept_from) {
    started_ms_ = now_ms();
    if (!resolve()) disabled_ = true;
}

SteamTransport::~SteamTransport() {
    if (ready_ && !disabled_ && peer_) {
        const auto id = identity(peer_);
        bool ignored = false;
        call(ignored, api_->close, api_->msgs, &id);
    }
    if (!disabled_) call_void(api_->release_thread_memory);
}

void SteamTransport::fail(const char* what) {
    if (!disabled_) log::error("Steam: exception in {}; Steam transport disabled", what);
    disabled_ = true;
    ready_ = false;
}

bool SteamTransport::resolve() {
    // The game links steam_api64.dll statically: it is loaded already. Never
    // LoadLibrary a second copy.
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    if (!dll) {
        log::error("Steam: steam_api64.dll is not loaded in this process");
        return false;
    }
    Api& a = *api_;
    int missing = 0;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
        if (!fn) {
            log::error("Steam: export {} not found", name);
            ++missing;
        }
    };
    get(a.get_hsteam_user, "SteamAPI_GetHSteamUser");
    get(a.release_thread_memory, "SteamAPI_ReleaseCurrentThreadMemory");
    get(a.messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    get(a.utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    get(a.user, "SteamAPI_SteamUser_v023");
    get(a.friends, "SteamAPI_SteamFriends_v017");
    get(a.send_to_user, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    get(a.receive, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    get(a.accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    get(a.close, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    get(a.session_info, "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");
    get(a.message_release, "SteamAPI_SteamNetworkingMessage_t_Release");
    get(a.init_relay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    get(a.relay_status, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    get(a.set_global_int, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
    get(a.get_steam_id, "SteamAPI_ISteamUser_GetSteamID");
    get(a.logged_on, "SteamAPI_ISteamUser_BLoggedOn");
    get(a.persona_name, "SteamAPI_ISteamFriends_GetPersonaName");
    get(a.friend_count, "SteamAPI_ISteamFriends_GetFriendCount");
    get(a.friend_by_index, "SteamAPI_ISteamFriends_GetFriendByIndex");
    get(a.friend_name, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    get(a.friend_game, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    if (missing) return false;
    log::info("Steam: flat API resolved ({} exports)", 22);
    return true;
}

void SteamTransport::become_ready() {
    Api& a = *api_;
    int32_t hu = 0;
    if (!call(hu, a.get_hsteam_user)) return fail("GetHSteamUser");
    if (hu == 0) return;  // the game has not initialised Steam yet
    if (!call(a.usr, a.user) || !call(a.frn, a.friends) || !call(a.msgs, a.messages) ||
        !call(a.utl, a.utils)) {
        return fail("interface accessors");
    }
    if (!a.usr || !a.frn || !a.msgs || !a.utl) return;
    bool on = false;
    if (!call(on, a.logged_on, a.usr)) return fail("BLoggedOn");
    if (!on) return;
    if (!call(own_id_, a.get_steam_id, a.usr)) return fail("GetSteamID");
    const char* name = nullptr;
    call(name, a.persona_name, a.frn);
    // Relay only (Valve's servers): with ICE, two friends may connect directly
    // and see each other's IP address. k_ESteamNetworkingConfig_
    // P2P_Transport_ICE_Enable (104) = 0 (disable).
    bool ice_off = false;
    call(ice_off, a.set_global_int, a.utl, 104, int32_t{0});
    log::info("Steam: direct connections {}", ice_off ? "off (relay only: the other player never sees your IP)"
                                                      : "could not be turned off");
    if (!call_void(a.init_relay, a.utl)) return fail("InitRelayNetworkAccess");
    ready_ = true;
    log::info("Steam: ready as '{}' (SteamID64 {}), channel {}, {}", name ? name : "?", own_id_,
              channel_, host_ ? "host" : "guest");
    const auto playing = friends_playing();
    if (playing.empty()) {
        log::info("Steam: no friends are playing Crimson Desert right now");
    }
    for (const auto& f : playing) {
        log::info("Steam: friend playing Crimson Desert: '{}' (SteamID64 {})", f.name, f.id);
    }
}

void SteamTransport::poll(uint32_t now_ms) {
    if (disabled_ || now_ms - last_poll_ms_ < kPollEveryMs) return;
    last_poll_ms_ = now_ms;
    Api& a = *api_;
    if (!ready_) {
        become_ready();
        if (!ready_ && !disabled_ && now_ms - started_ms_ > kReadyTimeoutMs) {
            log::error("Steam: not logged on after {} s (offline mode?); no co-op over Steam",
                       kReadyTimeoutMs / 1000);
            disabled_ = true;
        }
        return;
    }
    if (!call_void(a.release_thread_memory)) return fail("ReleaseCurrentThreadMemory");

    // Host: accept the session of whoever may connect. Sending to someone
    // accepts implicitly, so a guest needs nothing here.
    if (host_) {
        if (accept_from_ || peer_) {
            // Once playing with someone, nobody else's session is accepted.
            const auto id = identity(accept_from_ ? accept_from_ : peer_);
            bool ok = false;
            if (!call(ok, a.accept, a.msgs, &id)) return fail("AcceptSessionWithUser");
        } else {
            for (const auto& f : friends_playing()) {
                const auto id = identity(f.id);
                bool ok = false;
                if (!call(ok, a.accept, a.msgs, &id)) return fail("AcceptSessionWithUser");
            }
        }
    }

    int relay = 0;
    if (!call(relay, a.relay_status, a.utl, static_cast<void*>(nullptr))) return fail("GetRelayNetworkStatus");
    if (relay != last_relay_status_) {
        log::info("Steam: relay network status {} (100 = ready)", relay);
        last_relay_status_ = relay;
    }
    if (peer_) {
        const auto id = identity(peer_);
        int state = 0;
        RealTimeStatus rt{};
        if (!call(state, a.session_info, a.msgs, &id, static_cast<void*>(nullptr), static_cast<void*>(&rt))) {
            return fail("GetSessionConnectionInfo");
        }
        if (state == 3) {
            last_status_ = std::format("ping {} ms, out {:.1f} KB/s ({:.0f} pkt/s), in {:.1f} KB/s, send rate {} KB/s, "
                                       "pending {} B, queue {} ms",
                                       rt.ping_ms, rt.out_bytes_per_sec / 1024.0f, rt.out_packets_per_sec,
                                       rt.in_bytes_per_sec / 1024.0f, rt.send_rate_bytes_per_sec / 1024,
                                       rt.pending_unreliable + rt.pending_reliable, rt.queue_time_usec / 1000);
        }
        if (state != last_session_state_) {
            static const char* kNames[] = {"none", "connecting", "finding route", "connected",
                                           "closed by peer", "problem detected locally"};
            log::info("Steam: session with {} is {}", peer_,
                      state >= 0 && state <= 5 ? kNames[state] : std::to_string(state).c_str());
            last_session_state_ = state;
        }
    }
}

std::vector<SteamFriend> SteamTransport::friends_playing() {
    std::vector<SteamFriend> out;
    if (!ready_ || disabled_) return out;
    Api& a = *api_;
    int n = 0;
    if (!call(n, a.friend_count, a.frn, kFriendFlagImmediate)) {
        fail("GetFriendCount");
        return out;
    }
    for (int i = 0; i < n && i < 2000; ++i) {
        uint64_t id = 0;
        if (!call(id, a.friend_by_index, a.frn, i, kFriendFlagImmediate)) {
            fail("GetFriendByIndex");
            return {};
        }
        FriendGameInfo info{};
        bool playing = false;
        if (!call(playing, a.friend_game, a.frn, id, &info)) {
            fail("GetFriendGamePlayed");
            return {};
        }
        if (!playing || (info.game_id & 0xFFFFFF) != kAppId) continue;
        const char* name = nullptr;
        call(name, a.friend_name, a.frn, id);
        out.push_back({id, name ? name : "?"});
    }
    return out;
}

bool SteamTransport::send(const PeerAddr& to, std::span<const uint8_t> data) {
    if (!ready_ || disabled_ || to.kind != PeerAddr::Kind::Steam) return false;
    const auto id = identity(to.value);
    int32_t result = 0;
    if (!call(result, api_->send_to_user, api_->msgs, &id, static_cast<const void*>(data.data()),
              static_cast<uint32_t>(data.size()), kSendUnreliableNoNagle | kSendAutoRestartBrokenSession,
              channel_)) {
        fail("SendMessageToUser");
        return false;
    }
    if (result != kResultOK) {
        if (send_errors_++ % 100 == 0) {
            log::warn("Steam: SendMessageToUser returned EResult {} ({} errors)", result, send_errors_);
        }
        return false;
    }
    ++sent_;
    return true;
}

void SteamTransport::drain_incoming() {
    SteamNetworkingMessage* msgs[16];
    int n = 0;
    if (!call(n, api_->receive, api_->msgs, channel_, static_cast<SteamNetworkingMessage**>(msgs), 16)) {
        return fail("ReceiveMessagesOnChannel");
    }
    for (int i = 0; i < n && i < 16; ++i) {
        SteamNetworkingMessage* m = msgs[i];
        // Once playing with someone, messages from anyone else are dropped
        // here: they could crowd the peer's out of the inbox.
        if (m && m->peer.type == kIdentitySteamID && m->size > 0 &&
            static_cast<size_t>(m->size) <= proto::kMaxPacketSize && m->data &&
            (!peer_ || m->peer.steam_id64 == peer_)) {
            const auto* b = static_cast<const uint8_t*>(m->data);
            inbox_.emplace_back(m->peer.steam_id64, std::vector<uint8_t>(b, b + m->size));
            ++received_;
        }
        if (m && !call_void(api_->message_release, m)) return fail("SteamNetworkingMessage_t_Release");
    }
    while (inbox_.size() > 1024) inbox_.pop_front();
}

std::optional<size_t> SteamTransport::recv(PeerAddr& from, std::span<uint8_t> buf, int timeout_ms) {
    const uint32_t until = now_ms() + static_cast<uint32_t>(timeout_ms);
    for (;;) {
        if (inbox_.empty() && ready_ && !disabled_) drain_incoming();
        if (!inbox_.empty()) {
            auto [id, bytes] = std::move(inbox_.front());
            inbox_.pop_front();
            if (bytes.size() > buf.size()) continue;
            std::memcpy(buf.data(), bytes.data(), bytes.size());
            from = PeerAddr::steam(id);
            return bytes.size();
        }
        if (static_cast<int32_t>(now_ms() - until) >= 0) return std::nullopt;
        Sleep(1);
    }
}

std::string SteamTransport::status() const {
    if (disabled_) return "steam: disabled (see earlier errors)";
    if (!ready_) return "steam: waiting for the game's Steam login";
    return std::format("steam: id {} | sent {} recv {} send errors {} | session {} | relay {} | {}", own_id_,
                       sent_, received_, send_errors_, last_session_state_, last_relay_status_, last_status_);
}

}  // namespace cdmp::net
