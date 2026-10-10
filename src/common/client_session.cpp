#include "common/client_session.h"

#include <cstdio>
#include <random>

namespace cdmp {

ClientSession::ClientSession(std::string player_name, uint32_t mod_build, SendFn send,
                             EventFn on_event, Role role)
    : player_name_(std::move(player_name)),
      mod_build_(mod_build),
      role_(role),
      nonce_(std::random_device{}() | 1u),
      send_fn_(std::move(send)),
      event_fn_(std::move(on_event)),
      reliable_([this](proto::Body body) { this->send(std::move(body), now_for_reliable_); }) {}

void ClientSession::tick(uint32_t now_ms) {
    if (state_ == State::Connecting) {
        if (role_ == Role::Responder) return;  // a host waits to be greeted
        // Keep saying Hello until the peer answers. A single Hello can be
        // lost (peer not started yet, packet dropped), so never send it once.
        if (!started_ || now_ms - last_hello_ms_ >= kHelloRetryMs) {
            started_ = true;
            last_hello_ms_ = now_ms;
            send(proto::Hello{player_name_, mod_build_, nonce_}, now_ms);
        }
        return;
    }

    if (now_ms - last_recv_ms_ >= kTimeoutMs) {
        event("peer timed out after " + std::to_string(kTimeoutMs) + " ms; reconnecting");
        reset_to_connecting(now_ms);
        return;
    }

    if (now_ms - last_ping_ms_ >= kPingIntervalMs) {
        last_ping_ms_ = now_ms;
        send(proto::Ping{now_ms}, now_ms);
    }
    now_for_reliable_ = now_ms;
    reliable_.tick(now_ms);
}

void ClientSession::send_event(proto::GameEvent event, uint32_t now_ms) {
    if (state_ != State::Connected) return;
    send(std::move(event), now_ms);
    ++stats_.events_sent;
}

bool ClientSession::send_reliable(proto::RelKind kind, const std::vector<uint8_t>& bytes,
                                  uint32_t now_ms) {
    if (state_ != State::Connected) return false;
    now_for_reliable_ = now_ms;
    return reliable_.send(kind, bytes, now_ms);
}

void ClientSession::send_local_state(const proto::PlayerState& state, uint32_t now_ms) {
    if (state_ == State::Connected) send(state, now_ms);
}

void ClientSession::disconnect(uint32_t now_ms) {
    if (state_ == State::Connected) send(proto::Bye{}, now_ms);
    reset_to_connecting(now_ms);
}

// A Hello/Welcome of another protocol version: never connect (nothing else
// would decode), but say so, at most every 10 s.
bool ClientSession::other_version(const proto::Packet& packet, uint32_t now_ms) {
    if (packet.version == proto::kVersion) return false;
    const std::string* name = nullptr;
    if (auto* h = std::get_if<proto::Hello>(&packet.body)) name = &h->name;
    if (auto* w = std::get_if<proto::Welcome>(&packet.body)) name = &w->name;
    peer_problem_ = "'" + (name ? *name : std::string("?")) + "' uses protocol " + std::to_string(packet.version) +
                    ", we use " + std::to_string(proto::kVersion) + ": both need the same mod version";
    if (last_problem_event_ms_ == 0 || now_ms - last_problem_event_ms_ >= 10000) {
        last_problem_event_ms_ = now_ms;
        event("cannot connect: peer " + peer_problem_);
    }
    return true;
}

void ClientSession::check_build(const std::string& name, uint32_t build) {
    if (build == 0 || build == mod_build_) {  // 0: cdmp_peer
        peer_problem_.clear();
        return;
    }
    char ours[16], theirs[16];
    std::snprintf(ours, sizeof ours, "%08X", mod_build_);
    std::snprintf(theirs, sizeof theirs, "%08X", build);
    peer_problem_ = "'" + name + "' runs mod build " + theirs + ", we run " + ours + ": update both to the same";
    event("warning: peer " + peer_problem_);
}

void ClientSession::on_datagram(std::span<const uint8_t> bytes, uint32_t now_ms) {
    auto packet = proto::decode(bytes);
    if (!packet) {
        ++stats_.rejected;
        return;
    }
    ++stats_.received;
    if (other_version(*packet, now_ms)) {
        ++stats_.rejected;
        return;
    }

    if (auto* hello = std::get_if<proto::Hello>(&packet->body)) {
        send(proto::Welcome{player_name_, nonce_, mod_build_}, now_ms);
        if (state_ == State::Connected && hello->nonce == peer_nonce_) {
            last_recv_ms_ = now_ms;  // a duplicate of the Hello we already answered
            return;
        }
        if (state_ == State::Connected && restarted_ && now_ms - last_restart_ms_ < kMinRestartMs) {
            return;  // restarting again so soon: ignored
        }
        last_restart_ms_ = now_ms;
        restarted_ = true;
        check_build(hello->name, hello->mod_build);
        peer_nonce_ = hello->nonce;
        if (state_ == State::Connected) {
            // Only a side in Connecting says Hello: the other end restarted.
            event("peer '" + hello->name + "' reconnected");
            remote_state_.reset();
            reliable_.reset();
        } else {
            event("connected to peer '" + hello->name + "'");
        }
        peer_name_ = hello->name;
        state_ = State::Connected;
        last_ping_ms_ = now_ms;
        last_recv_ms_ = now_ms;
        ++epoch_;
        return;
    }

    if (auto* welcome = std::get_if<proto::Welcome>(&packet->body)) {
        // A host is greeted with Hello; a Welcome to it can only be forged.
        if (role_ == Role::Responder) {
            ++stats_.rejected;
            return;
        }
        if (state_ == State::Connecting) {
            check_build(welcome->name, welcome->mod_build);
            peer_name_ = welcome->name;
            peer_nonce_ = welcome->nonce;
            state_ = State::Connected;
            last_ping_ms_ = now_ms;
            ++epoch_;
            event("connected to peer '" + peer_name_ + "'");
        } else if (welcome->name != peer_name_ && (!restarted_ || now_ms - last_restart_ms_ >= kMinRestartMs)) {
            last_restart_ms_ = now_ms;
            restarted_ = true;
            // A relay announces who is on the other side when that changes:
            // a new partner, so nothing of the old one's reliable state applies.
            peer_name_ = welcome->name;
            remote_state_.reset();
            reliable_.reset();
            ++epoch_;
            event("peer is now '" + peer_name_ + "'");
        }
        last_recv_ms_ = now_ms;
        return;
    }

    // Everything else only counts once the handshake is done.
    if (state_ != State::Connected) {
        ++stats_.rejected;
        return;
    }
    last_recv_ms_ = now_ms;

    if (auto* pong = std::get_if<proto::Pong>(&packet->body)) {
        const float sample = static_cast<float>(now_ms - pong->origin_ms);
        stats_.rtt_ms = stats_.rtt_ms == 0.0f ? sample : stats_.rtt_ms * 0.8f + sample * 0.2f;
    } else if (auto* state = std::get_if<proto::PlayerState>(&packet->body)) {
        remote_state_ = *state;
        ++stats_.remote_states;
    } else if (auto* ev = std::get_if<proto::GameEvent>(&packet->body)) {
        ++stats_.events_received;
        if (static_cast<uint8_t>(ev->kind) > proto::kMaxEventKind) {
            ++stats_.unknown_kinds;  // a newer peer's event
        } else if (game_event_fn_) {
            game_event_fn_(*ev, now_ms);
        }
    } else if (auto* data = std::get_if<proto::RelData>(&packet->body)) {
        now_for_reliable_ = now_ms;
        for (auto& msg : reliable_.on_data(*data)) {
            ++stats_.reliable_received;
            if (static_cast<uint8_t>(msg.kind) > proto::kMaxRelKind) {
                ++stats_.unknown_kinds;  // acknowledged by the channel, dropped here
            } else if (reliable_fn_) {
                reliable_fn_(msg, now_ms);
            }
        }
    } else if (auto* ack = std::get_if<proto::RelAck>(&packet->body)) {
        reliable_.on_ack(*ack);
    } else if (std::holds_alternative<proto::Ping>(packet->body)) {
        send(proto::Pong{std::get<proto::Ping>(packet->body).origin_ms}, now_ms);
    } else if (std::holds_alternative<proto::Bye>(packet->body)) {
        event("peer said goodbye; reconnecting");
        reset_to_connecting(now_ms);
    }
}

void ClientSession::send(proto::Body body, uint32_t now_ms) {
    proto::Packet packet;
    packet.seq = next_seq_++;
    packet.send_ms = now_ms;
    packet.body = std::move(body);
    send_fn_(proto::encode(packet));
    ++stats_.sent;
}

void ClientSession::event(const std::string& text) const {
    if (event_fn_) event_fn_(text);
}

void ClientSession::reset_to_connecting(uint32_t now_ms) {
    state_ = State::Connecting;
    // A new nonce: the other side sees a reconnection and resets its side too
    // (otherwise its reliable data in flight was acknowledged and lost).
    nonce_ = std::random_device{}() | 1u;
    peer_name_.clear();
    remote_state_.reset();
    reliable_.reset();
    // Retry Hello on the next tick rather than waiting a full interval.
    started_ = false;
    last_hello_ms_ = now_ms;
}

}  // namespace cdmp
