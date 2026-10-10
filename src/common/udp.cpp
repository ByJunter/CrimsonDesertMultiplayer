#include "common/udp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <atomic>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace cdmp::net {
namespace {

std::atomic<int> g_startup_refs{0};

SOCKET as_socket(uintptr_t s) { return static_cast<SOCKET>(s); }

sockaddr_in to_sockaddr(const Endpoint& ep) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ep.ip;
    sa.sin_port = htons(ep.port);
    return sa;
}

}  // namespace

std::optional<Endpoint> Endpoint::resolve(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return std::nullopt;
    Endpoint ep;
    ep.ip = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
    ep.port = port;
    freeaddrinfo(result);
    return ep;
}

Endpoint Endpoint::loopback(uint16_t port) {
    return Endpoint{htonl(INADDR_LOOPBACK), port};
}

bool Endpoint::is_loopback() const {
    return (ntohl(ip) >> 24) == 127;
}

std::string Endpoint::str() const {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr addr{};
    addr.s_addr = ip;
    inet_ntop(AF_INET, &addr, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(port);
}

bool startup() {
    if (g_startup_refs.fetch_add(1) > 0) return true;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        g_startup_refs.fetch_sub(1);
        return false;
    }
    return true;
}

void cleanup() {
    if (g_startup_refs.fetch_sub(1) == 1) WSACleanup();
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint16_t port, bool loopback_only) {
    close();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    // Without this, Windows reports an ICMP "port unreachable" from an
    // earlier send as WSAECONNRESET on the next recvfrom, which would look
    // like a dead socket whenever the peer isn't running yet.
    BOOL report_reset = FALSE;
    DWORD unused = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report_reset, sizeof report_reset, nullptr, 0, &unused,
             nullptr, nullptr);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    sock_ = static_cast<uintptr_t>(s);
    return true;
}

void UdpSocket::close() {
    if (is_open()) {
        closesocket(as_socket(sock_));
        sock_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
}

bool UdpSocket::is_open() const { return as_socket(sock_) != INVALID_SOCKET; }

uint16_t UdpSocket::local_port() const {
    if (!is_open()) return 0;
    sockaddr_in sa{};
    int len = sizeof sa;
    if (getsockname(as_socket(sock_), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
}

bool UdpSocket::send_to(const Endpoint& to, std::span<const uint8_t> data) {
    if (!is_open()) return false;
    const sockaddr_in sa = to_sockaddr(to);
    const int sent = sendto(as_socket(sock_), reinterpret_cast<const char*>(data.data()),
                            static_cast<int>(data.size()), 0,
                            reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
    return sent == static_cast<int>(data.size());
}

std::optional<size_t> UdpSocket::recv_from(Endpoint& from, std::span<uint8_t> buf,
                                           int timeout_ms) {
    if (!is_open()) return std::nullopt;

    WSAPOLLFD pfd{};
    pfd.fd = as_socket(sock_);
    pfd.events = POLLRDNORM;
    if (WSAPoll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLRDNORM)) return std::nullopt;

    sockaddr_in sa{};
    int len = sizeof sa;
    const int n = recvfrom(as_socket(sock_), reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&sa),
                           &len);
    if (n == SOCKET_ERROR) return std::nullopt;
    from.ip = sa.sin_addr.s_addr;
    from.port = ntohs(sa.sin_port);
    return static_cast<size_t>(n);
}

std::string UdpSocket::last_error() {
    return "WSA error " + std::to_string(WSAGetLastError());
}

}  // namespace cdmp::net
