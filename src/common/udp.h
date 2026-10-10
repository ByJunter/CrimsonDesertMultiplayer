#pragma once
// Minimal non-blocking-ish UDP socket on top of Winsock. Used by both the mod
// and the peer simulator. Plain UDP is the development transport (works on a
// single PC over loopback); Steam networking will be added later behind the
// same send/receive shape.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace cdmp::net {

// IPv4 endpoint. `ip` is in network byte order, `port` in host byte order.
struct Endpoint {
    uint32_t ip = 0;
    uint16_t port = 0;

    static std::optional<Endpoint> resolve(const std::string& host, uint16_t port);
    static Endpoint loopback(uint16_t port);
    bool is_loopback() const;
    std::string str() const;
    bool operator==(const Endpoint&) const = default;
};

// Call once per process before using sockets (ref-counted, safe to nest).
bool startup();
void cleanup();

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // port 0 = ephemeral. loopback_only keeps the socket off the LAN and
    // avoids the Windows firewall prompt during single-PC development.
    bool open(uint16_t port, bool loopback_only);
    void close();
    bool is_open() const;
    uint16_t local_port() const;

    bool send_to(const Endpoint& to, std::span<const uint8_t> data);

    // Waits up to timeout_ms for one datagram. Returns its size, or nullopt
    // on timeout/error.
    std::optional<size_t> recv_from(Endpoint& from, std::span<uint8_t> buf, int timeout_ms);

    // Last Winsock error as text, for logging.
    static std::string last_error();

private:
    uintptr_t sock_ = ~uintptr_t{0};  // INVALID_SOCKET without pulling in winsock2.h
};

}  // namespace cdmp::net
