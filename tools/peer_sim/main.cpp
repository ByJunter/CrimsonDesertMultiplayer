// cdmp_peer.exe - stands in for the second player so everything can be
// tested on a single PC, and relays between two real players.
//
//   cdmp_peer [--port 37015] [--mode relay|ghost|bot] [--delay 100] [--offset 2]
//             [--loss 0] [--jitter 0] [--public] [--quiet] [--seconds N]
//
// relay: co-op. With two players connected, everything one sends (state,
//        game events, reliable data) is forwarded to the other. With a single
//        player it is echoed back after --delay ms, so the game shows a remote
//        copy of you driven over the network (PlayerState also shifted by
//        --offset metres on X for the overlay marker).
// ghost: echoes only your PlayerState, shifted by --offset (overlay test).
// bot:   ignores your movement and walks a 3 m circle around your last
//        known position (overlay test).
// --loss / --jitter simulate a bad connection (percent / ms) on everything
// this tool sends. --public listens on all interfaces (for a friend).

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "common/clock.h"
#include "common/protocol.h"
#include "common/udp.h"

using namespace cdmp;

namespace {

struct Options {
    uint16_t port = 37015;
    std::string mode = "relay";
    int delay_ms = 100;
    float offset_m = 2.0f;
    int loss_pct = 0;
    int jitter_ms = 0;
    bool public_bind = false;
    bool quiet = false;
    int seconds = 0;  // 0 = run until Ctrl+C
};

void usage() {
    std::printf(
        "Usage: cdmp_peer [--port N] [--mode relay|ghost|bot] [--delay MS] [--offset M]\n"
        "                 [--loss PCT] [--jitter MS] [--public] [--quiet] [--seconds N]\n");
}

std::optional<Options> parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--public") o.public_bind = true;
        else if (a == "--quiet") o.quiet = true;
        else if ((a == "--port") && (v = next())) o.port = static_cast<uint16_t>(std::atoi(v));
        else if ((a == "--mode") && (v = next())) o.mode = v;
        else if ((a == "--delay") && (v = next())) o.delay_ms = std::atoi(v);
        else if ((a == "--offset") && (v = next())) o.offset_m = static_cast<float>(std::atof(v));
        else if ((a == "--loss") && (v = next())) o.loss_pct = std::atoi(v);
        else if ((a == "--jitter") && (v = next())) o.jitter_ms = std::atoi(v);
        else if ((a == "--seconds") && (v = next())) o.seconds = std::atoi(v);
        else {
            usage();
            return std::nullopt;
        }
    }
    if (o.mode != "relay" && o.mode != "ghost" && o.mode != "bot") {
        usage();
        return std::nullopt;
    }
    return o;
}

volatile bool g_running = true;

BOOL WINAPI on_ctrl(DWORD) {
    g_running = false;
    return TRUE;
}

constexpr size_t kMaxClients = 2;
constexpr uint32_t kClientTimeoutMs = 5000;

class Peer {
public:
    explicit Peer(const Options& opt) : opt_(opt), rng_(std::random_device{}()) {}

    bool open() {
        if (!sock_.open(opt_.port, !opt_.public_bind)) {
            std::printf("Cannot bind UDP port %u: %s\n", opt_.port,
                        net::UdpSocket::last_error().c_str());
            return false;
        }
        std::printf("cdmp_peer listening on %s:%u, mode %s (delay %d ms, offset %.1f m, loss %d%%, "
                    "jitter %d ms)\n",
                    opt_.public_bind ? "0.0.0.0" : "127.0.0.1", opt_.port, opt_.mode.c_str(),
                    opt_.delay_ms, opt_.offset_m, opt_.loss_pct, opt_.jitter_ms);
        return true;
    }

    void run() {
        std::vector<uint8_t> buf(proto::kMaxPacketSize);
        const uint32_t started = now_ms();
        uint32_t last_bot = 0;
        uint32_t last_report = now_ms();

        while (g_running) {
            net::Endpoint from;
            if (auto n = sock_.recv_from(from, buf, 2)) handle({buf.data(), *n}, from);

            const uint32_t now = now_ms();
            flush_outbox(now);

            if (opt_.mode == "bot" && !clients_.empty() && now - last_bot >= 50) {
                last_bot = now;
                send_bot_state(clients_.front());
            }
            const size_t before = clients_.size();
            std::erase_if(clients_, [&](const Client& c) {
                if (now - c.last_recv_ms <= kClientTimeoutMs) return false;
                std::printf("Client '%s' %s timed out\n", c.name.c_str(), c.ep.str().c_str());
                return true;
            });
            if (clients_.size() != before) announce();
            if (!opt_.quiet && now - last_report >= 2000) {
                last_report = now;
                report();
            }
            if (opt_.seconds > 0 && now - started >= static_cast<uint32_t>(opt_.seconds) * 1000) {
                break;
            }
        }
        report();
    }

private:
    struct Client {
        net::Endpoint ep;
        std::string name;
        uint32_t last_recv_ms = 0;
        std::optional<proto::PlayerState> state;
        uint64_t received = 0, events = 0, reliable = 0;
    };
    struct Pending {
        uint32_t due_ms;
        net::Endpoint to;
        std::vector<uint8_t> bytes;
    };

    Client* find(const net::Endpoint& ep) {
        for (auto& c : clients_) {
            if (c.ep == ep) return &c;
        }
        return nullptr;
    }

    // Where a client's data goes: the other player, or back to itself.
    const Client& partner_of(const Client& c) const {
        for (const auto& o : clients_) {
            if (!(o.ep == c.ep)) return o;
        }
        return c;
    }

    void handle(std::span<const uint8_t> bytes, const net::Endpoint& from) {
        auto packet = proto::decode(bytes);
        if (!packet) {
            ++rejected_;
            return;
        }
        ++received_;

        if (auto* hello = std::get_if<proto::Hello>(&packet->body)) {
            Client* c = find(from);
            if (!c) {
                if (clients_.size() >= (opt_.mode == "relay" ? kMaxClients : 1)) {
                    ++rejected_;  // full: the client keeps retrying
                    return;
                }
                clients_.push_back({from, hello->name});
                c = &clients_.back();
                std::printf("Hello from '%s' (build %u) at %s [%zu player(s)]\n",
                            hello->name.c_str(), hello->mod_build, from.str().c_str(),
                            clients_.size());
            }
            c->last_recv_ms = now_ms();
            announce();
            return;
        }
        Client* c = find(from);
        if (!c) {
            ++rejected_;
            return;
        }
        c->last_recv_ms = now_ms();
        ++c->received;

        if (auto* ping = std::get_if<proto::Ping>(&packet->body)) {
            send(from, proto::Pong{ping->origin_ms});
        } else if (std::holds_alternative<proto::Bye>(packet->body)) {
            std::printf("Client '%s' %s said goodbye\n", c->name.c_str(), from.str().c_str());
            std::erase_if(clients_, [&](const Client& x) { return x.ep == from; });
            announce();
        } else if (auto* state = std::get_if<proto::PlayerState>(&packet->body)) {
            c->state = *state;
            if (opt_.mode == "bot") return;
            const Client& to = opt_.mode == "relay" ? partner_of(*c) : *c;
            proto::PlayerState out = *state;
            if (to.ep == c->ep) out.pos.x += opt_.offset_m;  // echo: keep the marker visible
            send(to.ep, out, opt_.delay_ms);
        } else if (opt_.mode == "relay") {
            // Game events and reliable fragments/acks: forwarded unchanged.
            if (std::holds_alternative<proto::GameEvent>(packet->body)) ++c->events;
            if (std::holds_alternative<proto::RelData>(packet->body)) ++c->reliable;
            send(partner_of(*c).ep, std::move(packet->body), opt_.delay_ms);
        }
    }

    // Welcome every client with the name of whoever is on the other side
    // (the other player in a relay, this tool otherwise). Sent again when
    // that changes; ClientSession picks up the new name.
    void announce() {
        for (const auto& c : clients_) {
            const Client& other = partner_of(c);
            const bool alone = opt_.mode != "relay" || other.ep == c.ep;
            send(c.ep, proto::Welcome{alone ? "cdmp_peer (" + opt_.mode + ")" : other.name});
        }
    }

    void send_bot_state(const Client& c) {
        const proto::Vec3 center = c.state ? c.state->pos : proto::Vec3{};
        const float angle = static_cast<float>(now_ms() % 6000) / 6000.0f * 6.2831853f;
        proto::PlayerState s;
        s.pos = {center.x + 3.0f * std::cos(angle), center.y, center.z + 3.0f * std::sin(angle)};
        s.rot = {0.0f, std::sin(angle * 0.5f), 0.0f, std::cos(angle * 0.5f)};
        s.flags = proto::kStateSynthetic;
        send(c.ep, s);
    }

    void send(const net::Endpoint& to, proto::Body body, int extra_delay_ms = 0) {
        proto::Packet p;
        p.seq = next_seq_++;
        p.send_ms = now_ms();
        p.body = std::move(body);
        if (opt_.loss_pct > 0 && static_cast<int>(rng_() % 100) < opt_.loss_pct) {
            ++dropped_;
            return;
        }
        int delay = extra_delay_ms;
        if (opt_.jitter_ms > 0) delay += static_cast<int>(rng_() % (opt_.jitter_ms + 1));
        outbox_.push_back({now_ms() + static_cast<uint32_t>(delay), to, proto::encode(p)});
    }

    void flush_outbox(uint32_t now) {
        // Jitter can reorder packets on purpose, so scan the whole queue.
        for (auto it = outbox_.begin(); it != outbox_.end();) {
            if (static_cast<int32_t>(now - it->due_ms) >= 0) {
                if (find(it->to)) {
                    sock_.send_to(it->to, it->bytes);
                    ++sent_;
                }
                it = outbox_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void report() {
        std::printf("recv %llu rejected %llu | sent %llu dropped %llu | %zu player(s)\n",
                    static_cast<unsigned long long>(received_),
                    static_cast<unsigned long long>(rejected_),
                    static_cast<unsigned long long>(sent_),
                    static_cast<unsigned long long>(dropped_), clients_.size());
        for (const auto& c : clients_) {
            std::string pos = "-";
            if (c.state) {
                char b[96];
                std::snprintf(b, sizeof b, "(%.2f, %.2f, %.2f) hp %.1f/%.1f%s", c.state->pos.x,
                              c.state->pos.y, c.state->pos.z, c.state->health,
                              c.state->health_max,
                              (c.state->flags & proto::kStateSynthetic) ? " synthetic" : "");
                pos = b;
            }
            std::printf("  '%s' %s: recv %llu, game events %llu, reliable fragments %llu, pos %s\n",
                        c.name.c_str(), c.ep.str().c_str(),
                        static_cast<unsigned long long>(c.received),
                        static_cast<unsigned long long>(c.events),
                        static_cast<unsigned long long>(c.reliable), pos.c_str());
        }
        std::fflush(stdout);
    }

    Options opt_;
    net::UdpSocket sock_;
    std::mt19937 rng_;
    std::vector<Client> clients_;
    std::deque<Pending> outbox_;
    uint32_t next_seq_ = 1;
    uint64_t received_ = 0, rejected_ = 0, sent_ = 0, dropped_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    auto opt = parse(argc, argv);
    if (!opt) return 2;
    if (!net::startup()) {
        std::printf("Winsock startup failed\n");
        return 1;
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    int rc = 1;
    {
        Peer peer(*opt);
        if (peer.open()) {
            peer.run();
            rc = 0;
        }
    }
    net::cleanup();
    return rc;
}
