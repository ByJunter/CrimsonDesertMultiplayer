// Unit tests for the protocol and the client session state machine.
// No framework: each CHECK prints the failing line and the run exits non-zero.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <limits>
#include <vector>

#include "common/byte_ring.h"
#include "common/client_session.h"
#include "common/clock_mapper.h"
#include "common/enemy_load.h"
#include "common/enemy_zone.h"
#include "common/equip_state.h"
#include "common/field_time.h"
#include "common/inject_clock.h"
#include "common/learnt_file.h"
#include "common/math3d.h"
#include "common/pattern.h"
#include "common/protocol.h"
#include "common/reliable.h"

using namespace cdmp;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

proto::Packet make(proto::Body body) {
    proto::Packet p;
    p.seq = 42;
    p.send_ms = 123456;
    p.body = std::move(body);
    return p;
}

void test_roundtrip() {
    {
        auto bytes = proto::encode(make(proto::Hello{"Dayi", 7}));
        auto p = proto::decode(bytes);
        CHECK(p && p->seq == 42 && p->send_ms == 123456);
        auto* h = p ? std::get_if<proto::Hello>(&p->body) : nullptr;
        CHECK(h && h->name == "Dayi" && h->mod_build == 7);
    }
    {
        proto::PlayerState s;
        s.pos = {1.5f, -2.0f, 3.25f};
        s.rot = {0.0f, 0.7071f, 0.0f, 0.7071f};
        s.health = 254.5f;
        s.health_max = 300.0f;
        s.flags = proto::kStateSynthetic;
        auto p = proto::decode(proto::encode(make(s)));
        auto* d = p ? std::get_if<proto::PlayerState>(&p->body) : nullptr;
        CHECK(d && d->pos.x == 1.5f && d->pos.y == -2.0f && d->pos.z == 3.25f);
        CHECK(d && d->health == 254.5f && d->health_max == 300.0f);
        CHECK(d && d->rot.y == 0.7071f && d->rot.w == 0.7071f);
        CHECK(d && d->flags == proto::kStateSynthetic);
    }
    for (proto::Body b : {proto::Body{proto::Welcome{"x"}}, proto::Body{proto::Bye{}},
                          proto::Body{proto::Ping{9}}, proto::Body{proto::Pong{9}}}) {
        auto p = proto::decode(proto::encode(make(b)));
        CHECK(p && proto::type_of(p->body) == proto::type_of(b));
    }
}

void test_rejects_bad_input() {
    auto good = proto::encode(make(proto::Ping{1}));

    auto bad_magic = good;
    bad_magic[0] ^= 0xFF;
    CHECK(!proto::decode(bad_magic));

    auto bad_version = good;
    bad_version[4] = 99;
    CHECK(!proto::decode(bad_version));

    auto bad_type = good;
    bad_type[6] = 200;
    CHECK(!proto::decode(bad_type));

    auto truncated = good;
    truncated.pop_back();
    CHECK(!proto::decode(truncated));

    CHECK(!proto::decode(std::vector<uint8_t>(5, 0)));
    CHECK(!proto::decode(std::vector<uint8_t>(proto::kMaxPacketSize + 1, 0)));

    proto::PlayerState nan_state;
    nan_state.pos.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!proto::decode(proto::encode(make(nan_state))));

    proto::PlayerState inf_state;
    inf_state.rot.w = std::numeric_limits<float>::infinity();
    CHECK(!proto::decode(proto::encode(make(inf_state))));

    // Long names are truncated on encode, and non-printable bytes sanitised.
    auto p = proto::decode(proto::encode(make(proto::Hello{std::string(100, 'a') , 1})));
    auto* h = p ? std::get_if<proto::Hello>(&p->body) : nullptr;
    CHECK(h && h->name.size() == proto::kMaxNameLen);
    p = proto::decode(proto::encode(make(proto::Hello{"a\nb", 1})));
    h = p ? std::get_if<proto::Hello>(&p->body) : nullptr;
    CHECK(h && h->name == "a?b");
}

struct Harness {
    std::vector<proto::Packet> sent;
    std::vector<std::string> events;
    ClientSession session{"tester", 1,
                          [this](const std::vector<uint8_t>& b) {
                              auto p = proto::decode(b);
                              if (p) sent.push_back(*p);
                          },
                          [this](const std::string& e) { events.push_back(e); }};

    void deliver(proto::Body body, uint32_t now) {
        session.on_datagram(proto::encode(make(std::move(body))), now);
    }
    size_t count(proto::Type t) const {
        size_t n = 0;
        for (auto& p : sent) n += proto::type_of(p.body) == t;
        return n;
    }
};

void test_session_handshake_and_retry() {
    Harness h;
    h.session.tick(1000);
    CHECK(h.count(proto::Type::Hello) == 1);
    h.session.tick(1500);
    CHECK(h.count(proto::Type::Hello) == 1);  // not yet time to retry
    h.session.tick(2000);
    CHECK(h.count(proto::Type::Hello) == 2);  // lost Hello is retried

    // States are not sent before the handshake completes.
    h.session.send_local_state({}, 2100);
    CHECK(h.count(proto::Type::PlayerState) == 0);

    // Nothing but Welcome is accepted while connecting.
    h.deliver(proto::PlayerState{}, 2150);
    CHECK(!h.session.remote_state());

    h.deliver(proto::Welcome{"peer"}, 2200);
    CHECK(h.session.state() == ClientSession::State::Connected);
    CHECK(h.session.peer_name() == "peer");

    h.session.send_local_state({}, 2300);
    CHECK(h.count(proto::Type::PlayerState) == 1);

    h.deliver(proto::PlayerState{{1, 2, 3}, {}, 0}, 2400);
    CHECK(h.session.remote_state() && h.session.remote_state()->pos.y == 2.0f);
}

void test_session_ping_rtt_and_timeout() {
    Harness h;
    h.session.tick(0);
    h.deliver(proto::Welcome{"peer"}, 10);
    h.session.tick(1010);
    CHECK(h.count(proto::Type::Ping) == 1);
    h.deliver(proto::Pong{1010}, 1050);
    CHECK(std::fabs(h.session.stats().rtt_ms - 40.0f) < 0.01f);

    // Peer pings us: we must answer with the same origin timestamp.
    h.deliver(proto::Ping{777}, 1100);
    CHECK(!h.sent.empty() && std::get_if<proto::Pong>(&h.sent.back().body) &&
          std::get<proto::Pong>(h.sent.back().body).origin_ms == 777);

    // Silence for kTimeoutMs drops back to Connecting and resumes Hellos.
    const size_t hellos = h.count(proto::Type::Hello);
    h.session.tick(1100 + ClientSession::kTimeoutMs);
    CHECK(h.session.state() == ClientSession::State::Connecting);
    h.session.tick(1101 + ClientSession::kTimeoutMs);
    CHECK(h.count(proto::Type::Hello) == hellos + 1);
}

void test_session_bye_and_garbage() {
    Harness h;
    h.session.tick(0);
    h.deliver(proto::Welcome{"peer"}, 10);
    h.session.on_datagram(std::vector<uint8_t>{1, 2, 3}, 20);
    CHECK(h.session.stats().rejected == 1);
    CHECK(h.session.state() == ClientSession::State::Connected);
    h.deliver(proto::Bye{}, 30);
    CHECK(h.session.state() == ClientSession::State::Connecting);
    CHECK(!h.events.empty());
}

void test_pattern() {
    CHECK(!parse_pattern(""));
    CHECK(!parse_pattern("?? ??"));  // all wildcards
    CHECK(!parse_pattern("4G"));
    CHECK(!parse_pattern("123"));
    auto p = parse_pattern("48 8b ?? ? C0");
    CHECK(p && p->size() == 5 && !p->mask[2] && !p->mask[3] && p->bytes[1] == 0x8B);

    const std::vector<uint8_t> data = {0x00, 0x48, 0x8B, 0x01, 0x02, 0xC0,
                                       0x48, 0x8B, 0xFF, 0xFF, 0xC0, 0x48};
    std::vector<size_t> hits;
    find_pattern(data, *p, hits, 10);
    CHECK(hits.size() == 2 && hits[0] == 1 && hits[1] == 6);

    hits.clear();
    find_pattern(data, *p, hits, 1);  // respects the limit
    CHECK(hits.size() == 1);

    // Leading wildcard: anchor is the first fixed byte, match at offset 0 too.
    auto lead = parse_pattern("?? 48 8B");
    hits.clear();
    find_pattern(data, *lead, hits, 10);
    CHECK(hits.size() == 2 && hits[0] == 0 && hits[1] == 5);

    // Pattern at the very end of the buffer, and longer than the buffer.
    auto tail = parse_pattern("C0 48");
    hits.clear();
    find_pattern(data, *tail, hits, 10);
    CHECK(hits.size() == 2 && hits[0] == 5 && hits[1] == 10);
    hits.clear();
    find_pattern(std::span(data).first(1), *tail, hits, 10);
    CHECK(hits.empty());
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

void test_math() {
    using math::quat_from_axes;
    auto id = quat_from_axes({1, 0, 0}, {0, 1, 0}, {0, 0, 1});
    CHECK(near(id.x, 0) && near(id.y, 0) && near(id.z, 0) && near(id.w, 1));

    // 90 degrees about Y (right-handed): X axis -> (0,0,-1), Z axis -> (1,0,0).
    auto yaw90 = quat_from_axes({0, 0, -1}, {0, 1, 0}, {1, 0, 0});
    CHECK(near(yaw90.x, 0) && near(yaw90.y, 0.70711f) && near(yaw90.z, 0) && near(yaw90.w, 0.70711f));

    // 180 degrees about Y takes the non-trace branch.
    auto yaw180 = quat_from_axes({-1, 0, 0}, {0, 1, 0}, {0, 0, -1});
    CHECK(near(std::fabs(yaw180.y), 1) && near(yaw180.w, 0));

    // Basis read live from the game (yaw about -28.6 degrees).
    proto::Vec3 gx{0.878f, 0.0f, 0.479f}, gy{0, 1, 0}, gz{-0.479f, 0.0f, 0.878f};
    CHECK(math::is_orthonormal(gx, gy, gz));
    auto g = quat_from_axes(gx, gy, gz);
    CHECK(near(g.x, 0) && near(g.z, 0) && std::fabs(g.y * g.y + g.w * g.w - 1) < 1e-3f);

    CHECK(!math::is_orthonormal({1, 0, 0}, {1, 0, 0}, {0, 0, 1}));
    CHECK(!math::is_orthonormal({2, 0, 0}, {0, 1, 0}, {0, 0, 1}));
    CHECK(!math::is_orthonormal({std::nanf(""), 0, 0}, {0, 1, 0}, {0, 0, 1}));
}

// Writes a record whose payload bytes all equal `fill`.
bool push(ByteRing& r, uint32_t len, uint8_t fill) {
    auto off = r.reserve(len);
    if (!off) return false;
    std::memset(r.at(*off), fill, len);
    std::memcpy(r.at(*off), &len, sizeof len);
    r.commit(len);
    return true;
}

void test_byte_ring() {
    std::vector<uint8_t> storage(100);
    ByteRing r(storage.data(), storage.size());
    std::vector<std::pair<uint32_t, uint8_t>> seen;
    auto drain = [&] {
        r.set_tail(r.drain(r.tail(), r.head(), [&](const uint8_t* rec, uint32_t len) {
            seen.emplace_back(len, rec[len - 1]);
        }));
    };

    CHECK(push(r, 40, 1));
    CHECK(push(r, 40, 2));
    CHECK(!push(r, 40, 3));  // only 20 bytes free
    drain();
    CHECK(seen.size() == 2 && seen[0] == std::make_pair(40u, uint8_t{1}) && seen[1].second == 2);

    // 20 bytes to the end: a 40-byte record must wrap (marker at offset 80).
    CHECK(push(r, 40, 3));
    CHECK(r.head() == 140);
    CHECK(push(r, 24, 4));
    drain();
    CHECK(seen.size() == 4 && seen[2] == std::make_pair(40u, uint8_t{3}) && seen[3].second == 4);

    // Fill exactly to the end of the ring, then wrap with < 4 bytes left.
    std::vector<uint8_t> s2(64);
    ByteRing r2(s2.data(), s2.size());
    seen.clear();
    CHECK(push(r2, 62, 7));   // 2 bytes left before the end
    r2.set_tail(r2.drain(r2.tail(), r2.head(), [&](const uint8_t* rec, uint32_t len) {
        seen.emplace_back(len, rec[len - 1]);
    }));
    CHECK(push(r2, 16, 8));   // wraps without a marker
    r2.set_tail(r2.drain(r2.tail(), r2.head(), [&](const uint8_t* rec, uint32_t len) {
        seen.emplace_back(len, rec[len - 1]);
    }));
    CHECK(seen.size() == 2 && seen[1] == std::make_pair(16u, uint8_t{8}));

    // A record larger than the whole ring is rejected.
    CHECK(!push(r2, 65, 9));

    // Long random run: interleave pushes and drains, verify order and content.
    std::vector<uint8_t> s3(4096);
    ByteRing r3(s3.data(), s3.size());
    uint32_t seed = 12345, next_fill = 0, expect_fill = 0, pushed = 0, drained = 0;
    bool order_ok = true;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1103515245 + 12345;
        const uint32_t len = 8 + (seed >> 16) % 600;
        if (push(r3, len, static_cast<uint8_t>(next_fill))) {
            ++next_fill;
            ++pushed;
        }
        if ((seed >> 8) % 3 == 0) {
            r3.set_tail(r3.drain(r3.tail(), r3.head(), [&](const uint8_t* rec, uint32_t l) {
                if (rec[l - 1] != static_cast<uint8_t>(expect_fill)) order_ok = false;
                ++expect_fill;
                ++drained;
            }));
        }
    }
    r3.set_tail(r3.drain(r3.tail(), r3.head(), [&](const uint8_t* rec, uint32_t l) {
        if (rec[l - 1] != static_cast<uint8_t>(expect_fill)) order_ok = false;
        ++expect_fill;
        ++drained;
    }));
    CHECK(order_ok);
    CHECK(pushed > 1000 && drained == pushed);
}

void test_coop_messages() {
    {
        proto::GameEvent ev;
        ev.kind = proto::EventKind::StartAction;
        ev.payload.resize(725);
        for (size_t i = 0; i < ev.payload.size(); ++i) ev.payload[i] = static_cast<uint8_t>(i * 7);
        auto p = proto::decode(proto::encode(make(ev)));
        auto* d = p ? std::get_if<proto::GameEvent>(&p->body) : nullptr;
        CHECK(d && d->kind == proto::EventKind::StartAction && d->payload == ev.payload);
    }
    {
        // The newest event kind decodes, and so does a newer one (a newer
        // peer's): the session drops it. Kind 0 never decodes.
        proto::GameEvent ev{proto::EventKind::GuestHit, std::vector<uint8_t>(17, 0x5A)};
        auto p = proto::decode(proto::encode(make(ev)));
        auto* d = p ? std::get_if<proto::GameEvent>(&p->body) : nullptr;
        CHECK(d && d->kind == proto::EventKind::GuestHit && d->payload == ev.payload);
        ev.kind = static_cast<proto::EventKind>(proto::kMaxEventKind + 1);
        CHECK(proto::decode(proto::encode(make(ev))));
        ev.kind = static_cast<proto::EventKind>(0);
        CHECK(!proto::decode(proto::encode(make(ev))));
    }
    {
        proto::RelData rd;
        rd.msg_id = 9;
        rd.index = 2;
        rd.count = 5;
        rd.kind = proto::RelKind::Spawn;
        rd.chunk.assign(proto::kRelChunk, 0xAB);
        auto bytes = proto::encode(make(rd));
        CHECK(bytes.size() <= proto::kMaxPacketSize);
        auto p = proto::decode(bytes);
        auto* d = p ? std::get_if<proto::RelData>(&p->body) : nullptr;
        CHECK(d && d->msg_id == 9 && d->index == 2 && d->count == 5 && d->chunk == rd.chunk);
        rd.index = 5;  // index >= count
        CHECK(!proto::decode(proto::encode(make(rd))));
    }
    {
        proto::GameEvent ev;
        ev.payload.resize(10);
        auto bytes = proto::encode(make(ev));
        bytes.push_back(0);  // trailing garbage
        CHECK(!proto::decode(bytes));
    }
    {
        proto::SpawnInfo info;
        info.generation = 3;
        info.user_key = 0x90100000;
        info.player_key = 0xA0100001;
        info.body_key = 0xB0100123;
        info.pos = {-10100.5f, 617.25f, -3530.0f};
        info.rot = {0.0f, 0.38f, 0.0f, 0.925f};
        info.user.assign(12666, 0x11);
        info.body.assign(18844, 0x22);
        info.equip = {4, 1, 0, 0x10, 0xA0, 0x10, 0x24, 0, 0, 0x55, 0x66, 0x77, 0x88, 0x10, 0};
        info.horse_key = 0xB0100003;
        info.horse_pos = {-10356.95f, 620.43f, -3467.32f};
        info.horse.assign(8929, 0x33);
        auto d = proto::decode_spawn(proto::encode_spawn(info));
        CHECK(d && d->generation == 3 && d->player_key == 0xA0100001 && d->body_key == 0xB0100123);
        CHECK(d && d->pos.y == 617.25f && d->user == info.user && d->body == info.body);
        CHECK(d && d->rot.y == 0.38f && d->equip == info.equip);
        CHECK(d && d->horse_key == 0xB0100003 && d->horse_pos.z == -3467.32f && d->horse == info.horse);
        auto bytes = proto::encode_spawn(info);
        bytes.pop_back();
        CHECK(!proto::decode_spawn(bytes));
    }
}

// Two channels connected through a lossy "wire": a large message must arrive
// exactly once despite drops, and the sender must end with nothing in flight.
void test_reliable_channel() {
    std::vector<proto::Body> a_to_b, b_to_a;
    ReliableChannel a([&](proto::Body b) { a_to_b.push_back(std::move(b)); });
    ReliableChannel b([&](proto::Body x) { b_to_a.push_back(std::move(x)); });

    std::vector<uint8_t> msg(31600);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i * 31 + 7);
    CHECK(a.send(proto::RelKind::Spawn, msg, 0));
    CHECK(a.unacked_fragments() == 29);

    uint32_t seed = 99;
    auto drop = [&] {
        seed = seed * 1103515245 + 12345;
        return (seed >> 16) % 100 < 30;  // 30% loss both ways
    };
    int delivered = 0;
    std::vector<uint8_t> got;
    for (uint32_t t = 0; t < 20000 && (a.unacked_fragments() > 0 || delivered == 0); t += 50) {
        auto ab = std::move(a_to_b);
        a_to_b.clear();
        for (auto& body : ab) {
            if (drop()) continue;
            // Every fragment is delivered twice to exercise duplicates.
            for (int rep = 0; rep < 2; ++rep) {
                for (auto& m : b.on_data(std::get<proto::RelData>(body))) {
                    ++delivered;
                    got = m.bytes;
                    CHECK(m.kind == proto::RelKind::Spawn);
                }
            }
        }
        auto ba = std::move(b_to_a);
        b_to_a.clear();
        for (auto& body : ba) {
            if (drop()) continue;
            a.on_ack(std::get<proto::RelAck>(body));
        }
        a.tick(t);
    }
    CHECK(delivered == 1);
    CHECK(got == msg);
    CHECK(a.unacked_fragments() == 0);
    CHECK(a.resent() > 0);
    CHECK(!a.send(proto::RelKind::Spawn, std::vector<uint8_t>(proto::kMaxRelMessage + 1), 0));
}

// Messages come out in the order they were sent whatever order their
// fragments arrive in; a receiver out of room refuses (and does not
// acknowledge) new messages instead of dropping acknowledged ones; a new
// stream (the sender reset) restarts the ordering and old stragglers are
// ignored.
// A peer that never sends the next message due cannot pile up complete later
// ones without limit (security review A1): past the budget they are refused,
// unacknowledged, and the next message due still gets in and frees them.
void test_reliable_budget() {
    size_t acked = 0;
    ReliableChannel r([&](proto::Body b) { acked += std::holds_alternative<proto::RelAck>(b); });
    auto frag = [](uint32_t id) {
        proto::RelData d;
        d.stream = 500;
        d.msg_id = id;
        d.index = 0;
        d.count = 1;
        d.kind = proto::RelKind::EnemyState;
        d.chunk.assign(proto::kRelChunk, 7);
        return d;
    };
    // The first fragment sets the stream: message 500 is the next one due.
    for (uint32_t i = 1; i <= ReliableChannel::kMaxHeldMessages + 10; ++i) CHECK(r.on_data(frag(500 + i)).empty());
    CHECK(acked == ReliableChannel::kMaxHeldMessages);
    CHECK(r.refused() == 10);
    CHECK(r.on_data(frag(500)).size() == ReliableChannel::kMaxHeldMessages + 1);
}

void test_reliable_order_and_streams() {
    std::vector<proto::Body> wire, acks;
    ReliableChannel a([&](proto::Body b) { wire.push_back(std::move(b)); });
    ReliableChannel b([&](proto::Body x) { acks.push_back(std::move(x)); });
    for (uint8_t i = 0; i < 5; ++i) CHECK(a.send(proto::RelKind::EnemyState, std::vector<uint8_t>(3000, i), 0));
    std::vector<proto::RelData> frags;
    for (auto& w : wire) frags.push_back(std::get<proto::RelData>(w));
    CHECK(frags.size() == 15);
    std::vector<uint8_t> order;
    for (auto it = frags.rbegin(); it != frags.rend(); ++it) {
        for (auto& m : b.on_data(*it)) order.push_back(m.bytes[0]);
    }
    CHECK((order == std::vector<uint8_t>{0, 1, 2, 3, 4}));

    // Room for kMaxPartials incomplete messages: the next one is refused, unacknowledged.
    acks.clear();
    ReliableChannel d([&](proto::Body x) { acks.push_back(std::move(x)); });
    size_t delivered = 0;
    for (uint32_t i = 0; i <= ReliableChannel::kMaxPartials; ++i) {
        proto::RelData half;  // the second half of a two-fragment message
        half.stream = 1001;
        half.msg_id = 1001 + i;
        half.index = 1;
        half.count = 2;
        half.chunk.assign(10, 1);
        delivered += d.on_data(half).size();
    }
    CHECK(delivered == 0);
    CHECK(d.refused() == 1);
    CHECK(acks.size() == ReliableChannel::kMaxPartials);

    // A reset sender starts a new stream: delivered from its first message on.
    ReliableChannel e([&](proto::Body x) { wire.push_back(std::move(x)); });
    ReliableChannel f([&](proto::Body) {});
    wire.clear();
    CHECK(e.send(proto::RelKind::Equip, {1}, 0));
    CHECK(e.send(proto::RelKind::Equip, {2}, 0));
    auto old_second = std::get<proto::RelData>(wire[1]);
    CHECK(f.on_data(std::get<proto::RelData>(wire[0])).size() == 1);
    e.reset();
    wire.clear();
    CHECK(e.send(proto::RelKind::Equip, {3}, 0));
    auto fresh = f.on_data(std::get<proto::RelData>(wire[0]));
    CHECK(fresh.size() == 1 && fresh[0].bytes[0] == 3);
    CHECK(f.on_data(old_second).empty());  // straggler of the old stream

    // The window: never more than kWindow fragments in flight.
    wire.clear();
    ReliableChannel g([&](proto::Body x) { wire.push_back(std::move(x)); });
    CHECK(g.send(proto::RelKind::Spawn, std::vector<uint8_t>(proto::kRelChunk * 200), 0));
    CHECK(wire.size() == ReliableChannel::kWindow);
    CHECK(g.unacked_fragments() == 200);
}

// A peer with another protocol version is recognised by its Hello/Welcome
// and reported; nothing else of it decodes, and we never connect.
void test_versions() {
    auto old = proto::encode(make(proto::Hello{"Friend", 3, 77}));
    old[4] = 5;  // version field (u16 at +4)
    old[5] = 0;
    auto p = proto::decode(old);
    auto* hello = p ? std::get_if<proto::Hello>(&p->body) : nullptr;
    CHECK(p && p->version == 5 && hello && hello->name == "Friend");
    auto ping = proto::encode(make(proto::Ping{1}));
    ping[4] = 5;
    CHECK(!proto::decode(ping));

    Harness h;
    h.session.tick(0);
    h.session.on_datagram(old, 10);
    CHECK(h.session.state() == ClientSession::State::Connecting);
    CHECK(h.session.peer_problem().find("protocol 5") != std::string::npos);
    CHECK(!h.events.empty() && h.events.back().find("cannot connect") != std::string::npos);

    // Same protocol, another build: connects, with a warning.
    Harness b;
    b.session.tick(0);
    b.deliver(proto::Welcome{"Friend", 5, 0xABCDEF01}, 10);
    CHECK(b.session.state() == ClientSession::State::Connected);
    CHECK(b.session.peer_problem().find("ABCDEF01") != std::string::npos);
    // cdmp_peer's relay (build 0) is not a mismatch.
    Harness c;
    c.session.tick(0);
    c.deliver(proto::Welcome{"cdmp_peer"}, 10);
    CHECK(c.session.state() == ClientSession::State::Connected && c.session.peer_problem().empty());

    // A newer peer's event kind is dropped, not handed to the game.
    int events = 0;
    c.session.set_game_handlers([&](const proto::GameEvent&, uint32_t) { ++events; },
                                [](const ReliableChannel::Message&, uint32_t) {});
    c.deliver(proto::GameEvent{static_cast<proto::EventKind>(200), {1, 2}}, 20);
    CHECK(events == 0 && c.session.stats().unknown_kinds == 1);
}

// The session hands game events and completed reliable messages to the
// handlers, and only while connected.
void test_session_game_data() {
    std::vector<std::vector<uint8_t>> out;
    ClientSession s("Me", 1, [&](const std::vector<uint8_t>& b) { out.push_back(b); });
    int events = 0, reliables = 0;
    s.set_game_handlers([&](const proto::GameEvent&, uint32_t) { ++events; },
                        [&](const ReliableChannel::Message&, uint32_t) { ++reliables; });
    s.tick(0);
    s.send_event(proto::GameEvent{}, 0);  // not connected: dropped
    CHECK(s.stats().events_sent == 0);
    s.on_datagram(proto::encode(make(proto::Welcome{"peer"})), 10);
    CHECK(s.state() == ClientSession::State::Connected);

    out.clear();
    s.send_event(proto::GameEvent{proto::EventKind::Move, std::vector<uint8_t>(514, 1)}, 20);
    CHECK(s.send_reliable(proto::RelKind::Despawn, {1, 0, 0, 0}, 20));
    CHECK(out.size() == 2);
    // Loop everything back, like cdmp_peer's relay with a single player.
    for (auto& b : std::vector<std::vector<uint8_t>>(out)) s.on_datagram(b, 30);
    for (auto& b : std::vector<std::vector<uint8_t>>(out.begin() + 2, out.end())) s.on_datagram(b, 31);
    CHECK(events == 1);
    CHECK(reliables == 1);
    CHECK(s.reliable_pending() == 0);
}

// Remote clock 50 s ahead, transit 30-80 ms: the offset converges to
// -50000 + ~30, moves at most 1 ms per sample afterwards, and jumps only for
// big changes.
void test_clock_mapper() {
    ClockMapper m;
    CHECK(!m.offset());
    uint32_t seed = 7;
    for (uint32_t t = 0; t < 3000; t += 25) {
        seed = seed * 1103515245 + 12345;
        const uint32_t transit = 30 + (seed >> 16) % 51;
        m.add(50000 + t + transit, 100000 + t, t);  // (our clock on arrival, remote ts)
    }
    CHECK(m.offset() && *m.offset() >= -50000 + 30 && *m.offset() <= -50000 + 40);
    CHECK(m.target() && *m.target() >= -50000 + 30 && *m.target() <= -50000 + 33);
    // A faster sample lowers the target at once but the offset only by 1 ms.
    const int64_t before = *m.offset();
    m.add(50000 + 3000 + 5, 100000 + 3000, 3000);
    CHECK(*m.target() == -50000 + 5);
    CHECK(*m.offset() == before - 1);
    // Steady 40 ms transit: the fast sample expires and the offset settles on it.
    for (uint32_t t = 3025; t < 3000 + ClockMapper::kWindowMs + 2000; t += 25) {
        m.add(50000 + t + 40, 100000 + t, t);
    }
    CHECK(*m.target() == -50000 + 40);
    CHECK(*m.offset() == -50000 + 40);
    // The other game paused for 2 s: re-estimated after ~0.5 s, not 4 s.
    for (uint32_t t = 9000; t < 9000 + 25 * ClockMapper::kLateRunToReset; t += 25) {
        m.add(50000 + t + 2040, 100000 + t, t);
    }
    CHECK(*m.offset() == -50000 + 2040);
    // The other game sat 19 s in a menu (silence), then resumes 19 s behind:
    // taken at the first sample, in either direction.
    m.add(50000 + 20000 + 2040 - 19000, 100000 + 20000 - 19000 + 0, 20000);
    CHECK(*m.offset() == -50000 + 2040);  // unchanged: no silence before it
    m.add(50000 + 25000 + 40 + 19000, 100000 + 25000, 25000);
    CHECK(*m.offset() == -50000 + 19040);
    m.add(50000 + 30000 + 40, 100000 + 30000, 30000);
    CHECK(*m.offset() == -50000 + 40);
    m.reset();
    CHECK(!m.offset() && !m.target());
}

// Injected movement timestamps stay within [now - 500, now + 300] of the
// client's clock and increase per buffer; the pause detector sees a stopped
// game clock.
void test_inject_clock() {
    using B = InjectClock::Buffer;
    InjectClock c;
    CHECK(c.stamp(1, B::Position, 9900, 10000) == 9900);            // normal lag: unchanged
    CHECK(c.stamp(1, B::Position, 10200, 10000) == 10200);          // ahead by the playout delay: unchanged
    CHECK(c.stamp(1, B::Position, 10000 + 25000, 10000) == 10450);  // 25 s ahead (receiver paused)
    CHECK(c.stamp(1, B::Position, 10450, 10050) == 0);              // repeated: dropped
    CHECK(c.stamp(1, B::Position, 10200, 10050) == 10451);          // far behind (a pause): kept, after it
    CHECK(c.stamp(1, B::Rotation, 10100, 10050) == 10100);          // its own buffer
    CHECK(c.stamp(1, B::Both, 10200, 10100) == 10200);              // StartAction behind the Moves: as is
    CHECK(c.stamp(1, B::Both, 10500, 10100) == 10500);              // ahead of both: as is
    CHECK(c.stamp(1, B::Position, 10520, 10100) == 10520);
    CHECK(c.stamp(1, B::Rotation, 10250, 10100) == 10250);          // behind: as is, never pulled forward
    CHECK(c.stamp(2, B::Position, 200000 - 144000, 200000) == 199500);  // 144 s behind
    c.forget(1);
    CHECK(c.stamp(1, B::Position, 11000, 11000) == 11000);
    CHECK(c.stamp(3, B::Position, 100, 200) == 100);  // no underflow near 0

    PauseDetector p;
    uint64_t game = 1000, wall = 50000;
    CHECK(!p.update(game, wall));
    // A 150 ms frame and a 300 ms step back of the clock: not a pause.
    for (int i = 0; i < 10; ++i) CHECK(!p.update(game += 16, wall += 16));
    CHECK(!p.update(game, wall += 150));
    CHECK(!p.update(game += 16, wall += 16));
    CHECK(!p.update(game -= 300, wall += 16));
    for (int i = 0; i < 40; ++i) CHECK(!p.update(game += 16, wall += 16));
    // A menu: the clock stops; paused within a second, running again at once.
    bool paused = false;
    for (int i = 0; i < 60; ++i) paused = p.update(game, wall += 16);
    CHECK(paused);
    for (int i = 0; i < 6; ++i) p.update(game += 16, wall += 16);
    CHECK(!p.update(game += 16, wall += 16));
}

// Gear changes since the templates, with the sequences of the two-PC test of
// 5 Oct (ropa-al-desmontar.md section 2): a reloaded copy must get its
// template's items removed before the new ones go on.
void test_equip_state() {
    using Items = EquipState::Items;
    using List = std::vector<std::pair<uint32_t, int>>;
    auto items = [](const List& l) {
        Items out;
        for (const auto& [item, slot] : l) out.emplace_back(item, static_cast<uint16_t>(slot));
        return out;
    };
    auto change = [&](uint32_t serial, const List& l) { return EquipState::payload(0xA0100001, serial, items(l)); };
    // Guest: took the helmet off (slot 3), then swapped slot 4 three times.
    EquipState g;
    CHECK(g.apply(change(0x2528, {{0, 3}})));
    for (uint32_t s = 0x2529; s <= 0x252B; ++s) CHECK(g.apply(change(s, {{0xF42E4, 4}, {0, 4}})));
    CHECK((g.removals() == std::vector<uint16_t>{3, 4}));
    CHECK((g.puts() == items({{0xF42E4, 4}})));
    CHECK(g.serial() == 0x252B);
    // Host: emptied slots 4 and 3, then put items on the empty slots.
    EquipState h;
    h.apply(change(9529, {{0, 4}}));
    h.apply(change(9530, {{0, 3}}));
    h.apply(change(9531, {{0xF4543, 3}}));
    h.apply(change(9532, {{0x1B23D, 4}}));
    CHECK((h.removals() == std::vector<uint16_t>{3, 4}));
    CHECK((h.puts() == items({{0xF4543, 3}, {0x1B23D, 4}})));
    // The snapshot reads back to the same state (the receiver merges into it).
    EquipState back;
    CHECK(back.apply(h.snapshot(0xA0100001)));
    CHECK(back.removals() == h.removals() && back.puts() == h.puts() && back.serial() == 9532);
    const auto snap = h.snapshot(0xA0100001);
    CHECK(snap.size() == 9 + 6 * 4 && snap[0] == 4);
    // A slot that was empty at load: put on, swapped, taken off -> nothing to replay.
    EquipState e;
    e.apply(change(1, {{0x111, 7}}));
    e.apply(change(2, {{0x222, 7}, {0, 7}}));
    CHECK(e.removals().empty() && (e.puts() == items({{0x222, 7}})));
    e.apply(change(3, {{0, 7}}));
    CHECK(e.removals().empty() && e.puts().empty());
    // Malformed payloads change nothing; the serial never goes back.
    const uint8_t bad[10] = {4};
    CHECK(!e.apply(bad, sizeof bad));
    e.apply(change(2, {{0x333, 8}}));
    CHECK(e.serial() == 3);
}

// pa::FieldTime raw conversions, with the worked examples of
// hora-del-dia.md section 8 (checked there against the game's instructions).
void test_field_time() {
    auto at = [](uint32_t day, uint32_t h, uint32_t m, uint32_t sec, uint32_t ms) {
        FieldTime f;
        f.day = day, f.hour = h, f.minute = m, f.second = sec, f.ms = ms;
        return f;
    };
    CHECK(at(10, 0, 0, 0, 0).to_raw() == 756'000'000);
    CHECK(at(10, 2, 59, 59, 999).to_raw() == 761'399'999);
    CHECK(at(10, 3, 0, 0, 0).to_raw() == 761'400'000);
    CHECK(at(10, 6, 0, 0, 0).to_raw() == 772'200'000);
    CHECK(at(10, 20, 59, 59, 999).to_raw() == 826'199'999);
    CHECK(at(10, 21, 0, 0, 0).to_raw() == 826'200'000);
    CHECK(at(10, 23, 30, 0, 0).to_raw() == 830'700'000);
    FieldTime f;
    f.set_raw(761'399'999);  // night: two ms per raw unit
    CHECK(f.day == 10 && f.hour == 2 && f.minute == 59 && f.second == 59 && f.ms == 998 && f.weekday == 3);
    f.set_raw(at(10, 23, 59, 59, 999).to_raw() + 1);
    CHECK(f.day == 11 && f.hour == 0 && f.minute == 0 && f.weekday == 4);
    for (uint32_t h : {3u, 6u, 12u, 20u}) {  // daytime round trip is exact
        FieldTime g;
        g.set_raw(at(7, h, 15, 30, 250).to_raw());
        CHECK(g.day == 7 && g.hour == h && g.minute == 15 && g.second == 30 && g.ms == 250);
    }
    // Carried forward like GetCurrentFieldTime: 10 s at scale 12 = 2 game minutes.
    FieldTime e = at(1, 20, 59, 0, 0);
    e.scale = 12, e.anchor = 1'000'000;
    FieldTime n;
    n.set_raw(e.raw_at(1'010'000));
    CHECK(n.hour == 21 && n.minute == 2 && n.second == 0);
    CHECK(e.raw_at(999'000) == e.to_raw());  // anchor in the future: frozen
    // Wire form: 31 bytes, the anchor last.
    uint8_t w[FieldTime::kWireSize] = {};
    e.to_wire(w);
    uint64_t anchor;
    std::memcpy(&anchor, w + 23, 8);
    CHECK(anchor == 1'000'000 && w[20] == e.weekday);
    // Their time of day on our nearest day: the day count moves half a day at most.
    constexpr uint64_t D = FieldTime::kRawDay;
    const uint64_t ours = 50 * D + at(0, 12, 0, 0, 0).to_raw();
    CHECK(nearest_raw_with_time_of(ours, 3 * D + at(0, 15, 0, 0, 0).to_raw()) == 50 * D + at(0, 15, 0, 0, 0).to_raw());
    CHECK(nearest_raw_with_time_of(ours, 3 * D + at(0, 6, 0, 0, 0).to_raw()) == 50 * D + at(0, 6, 0, 0, 0).to_raw());
    const uint64_t late = 50 * D + at(0, 23, 0, 0, 0).to_raw();
    CHECK(nearest_raw_with_time_of(late, at(0, 1, 0, 0, 0).to_raw()) == 51 * D + at(0, 1, 0, 0, 0).to_raw());
    const uint64_t early = 50 * D + at(0, 1, 0, 0, 0).to_raw();
    CHECK(nearest_raw_with_time_of(early, at(0, 23, 0, 0, 0).to_raw()) == 49 * D + at(0, 23, 0, 0, 0).to_raw());
}

// A host (Responder) and a guest (Initiator) wired back to back: the guest
// connects, the host learns its name, and a guest restart is noticed.
void test_session_host_guest() {
    std::vector<std::vector<uint8_t>> to_host, to_guest;
    ClientSession host("Host", 1, [&](const std::vector<uint8_t>& b) { to_guest.push_back(b); }, {},
                       ClientSession::Role::Responder);
    auto make_guest = [&] {
        return ClientSession("Guest", 1, [&](const std::vector<uint8_t>& b) { to_host.push_back(b); });
    };
    ClientSession guest = make_guest();
    auto pump = [&](uint32_t now) {
        for (int i = 0; i < 4; ++i) {
            auto a = std::move(to_host);
            to_host.clear();
            for (auto& b : a) host.on_datagram(b, now);
            auto c = std::move(to_guest);
            to_guest.clear();
            for (auto& b : c) guest.on_datagram(b, now);
        }
    };
    host.tick(0);
    CHECK(to_guest.empty());  // a host never says Hello on its own
    guest.tick(0);
    pump(0);
    CHECK(host.state() == ClientSession::State::Connected && host.peer_name() == "Guest");
    CHECK(guest.state() == ClientSession::State::Connected && guest.peer_name() == "Host");
    const uint32_t epoch = host.epoch();
    CHECK(epoch == 1);

    // Duplicate Hellos (Steam delivering a queue of them) are not restarts.
    ClientSession guest2("Guest", 1, [&](const std::vector<uint8_t>& b) { to_host.push_back(b); });
    guest2.tick(100);
    const auto hello = to_host;
    to_host.clear();
    for (auto& b : hello) host.on_datagram(b, 4000);  // the guest's game restarted: a new nonce
    CHECK(host.state() == ClientSession::State::Connected && host.epoch() == epoch + 1);
    for (auto& b : hello) {
        host.on_datagram(b, 4050);
        host.on_datagram(b, 4051);
    }
    CHECK(host.epoch() == epoch + 1);  // same nonce: no reset

    // A peer cannot restart the session in a loop (security review M1): a
    // new nonce within 3 s of the last restart is ignored.
    ClientSession guest3("Guest", 1, [&](const std::vector<uint8_t>& b) { to_host.push_back(b); });
    guest3.tick(4100);
    for (auto& b : std::vector<std::vector<uint8_t>>(std::move(to_host))) host.on_datagram(b, 4100);
    to_host.clear();
    CHECK(host.epoch() == epoch + 1);
    // A Welcome to a host is forged: ignored.
    proto::Packet w;
    w.body = proto::Welcome{"Someone", 7, 0};
    host.on_datagram(proto::encode(w), 9000);
    CHECK(host.peer_name() == "Guest" && host.epoch() == epoch + 1);
}

void test_learnt_file() {
    constexpr uint32_t kBuild = 0x6AB28F00;
    LearntCombat c;
    auto& a = c.attacks[0xF653A168ull << 32 | 0];
    a.base_votes = {{280, 40}, {560, 2}};  // a crit or two: the mode wins
    a.clean = 42, a.no_damage = 0, a.staggers = 40, a.blows = 42;
    c.attacks[0xE912512Dull << 32 | (3 << 8 | 1)].blows = 1;  // seen, never hurt yet
    c.defence[52671] = 0.486;
    c.enemy_hits[50031ull << 32 | 0x6A5683A7] = 30000;
    c.enemy_types[50031] = 30000;
    c.reactions[50031ull << 32 | 0xC1ECDAEC] = {5, 4};
    const std::string text = learnt::format(c, kBuild);
    const auto back = learnt::parse(text, kBuild);
    CHECK(back.has_value());
    if (back) {
        const auto& b = back->attacks.at(0xF653A168ull << 32 | 0);
        // Only the mode, with few votes: new gear takes over after a few blows.
        CHECK(b.base_votes.size() == 1 && b.base_votes.at(280) == learnt::kKeptVotes);
        CHECK(b.blows == learnt::kKeptBlows && b.staggers * 10 >= b.blows * 8 && b.clean <= b.blows);
        CHECK(back->attacks.count(0xE912512Dull << 32 | (3 << 8 | 1)) == 1);
        CHECK(std::fabs(back->defence.at(52671) - 0.486) < 1e-6);
        CHECK(back->enemy_hits.at(50031ull << 32 | 0x6A5683A7) == 30000);
        CHECK(back->enemy_types.at(50031) == 30000);
        CHECK(back->reactions.at(50031ull << 32 | 0xC1ECDAEC).blows == 5);
        CHECK(learnt::format(*back, kBuild) == learnt::format(*learnt::parse(learnt::format(*back, kBuild), kBuild), kBuild));
    }
    // Another game build, not our file, nothing: ignored.
    CHECK(!learnt::parse(text, kBuild + 1));
    CHECK(!learnt::parse("hello\nbuild 6AB28F00\n", kBuild));
    CHECK(!learnt::parse("", kBuild));
    // Lines out of range or malformed are skipped, the rest kept (CRLF too).
    const std::string odd =
        "cdmp-learnt 1\r\nbuild 6AB28F00\r\n"
        "attack F653A168 0 99999 3 1 0 0 1\n"     // 9,999.9 HP: no
        "attack F653A168 1 280 3 5 0 0 1\n"       // more clean blows than blows: no
        "attack F653A168 2 280 3 1 0 0 1 7\n"     // a field too many: no
        "attack ZZ 3 280 3 1 0 0 1\n"             // not hex: no
        "defence 50031 nan\n"
        "defence 50032 50\n"
        "enemyhit 50031 6A5683A7 -5\n"
        "enemytype 0 3000\n"
        "reaction 50031 C1ECDAEC 2 3\n"
        "attack F653A168 4 170 2 2 0 2 2\n"       // fine
        "defence 50033 0.857\n";                  // fine
    const auto o = learnt::parse(odd, kBuild);
    CHECK(o && o->attacks.size() == 1 && o->attacks.count(0xF653A168ull << 32 | 4) == 1);
    CHECK(o && o->defence.size() == 1 && o->defence.count(50033) == 1);
    CHECK(o && o->enemy_hits.empty() && o->enemy_types.empty() && o->reactions.empty());
    // An oversized file is refused whole.
    CHECK(!learnt::parse(std::string(learnt::kMaxFileBytes + 1, 'x'), kBuild));
}

// ---- phase 5: the enemy-zone predicate, its message and its state machine

// A one-record NPC load with the layout of build 2976 (enemy_load.h): header,
// name, T0 (after its signature) with its origin fields, a last Transform,
// then the Hierarchy, parent and children at the end.
struct LoadSpec {
    uint8_t type = 3;
    uint32_t key = 0xB0100223, owner = 0, variant = 50031, faction = 1000016;  // Faction_BleedBlade
    char origin = 'S';  // S schedule, Q sequence, F faction node, - none, ? no T0
    uint32_t origin_id = 0x343A74A6;  // an allowed schedule (cd_m03_m05_monster_creature_region_00_0001)
    uint32_t parent = 0;
    std::vector<uint32_t> children, hierarchy;
    proto::Vec3 pos{-10284.0f, 640.0f, -3383.0f};
};

std::vector<uint8_t> make_load(const LoadSpec& s) {
    std::vector<uint8_t> v(0x60, 0);
    auto put32 = [&](size_t at, uint32_t x) { std::memcpy(v.data() + at, &x, 4); };
    auto putf = [&](size_t at, float x) { std::memcpy(v.data() + at, &x, 4); };
    v[2] = s.type;
    put32(3, s.key);
    put32(7, s.owner);
    std::memset(v.data() + 12, 0xFF, 8);
    put32(0x18, s.variant);
    put32(0x1C, s.faction);
    const char* name = "Bleed Bandit";
    put32(0x20, static_cast<uint32_t>(std::strlen(name)));
    for (size_t i = 0; name[i]; ++i) v[0x24 + 2 * i] = static_cast<uint8_t>(name[i]);
    size_t t0 = 0;
    if (s.origin != '?') {
        const size_t sig = v.size();
        v.resize(sig + 18 + 1200, 0);
        v[sig] = 0x0C;
        std::memset(v.data() + sig + 10, 0xFF, 4);
        t0 = sig + 18;
        putf(t0, 1.0f), putf(t0 + 4, 2.0f), putf(t0 + 8, 3.0f);  // its spawn point
        putf(t0 + 24, 1.0f);                                        // rotation w
        putf(t0 + 28, 1.0f), putf(t0 + 32, 1.0f), putf(t0 + 36, 1.0f);
        if (s.origin == 'S') put32(t0 + 42, s.origin_id), put32(t0 + 46, 7);
        if (s.origin == 'F') put32(t0 + 390, s.origin_id), v[t0 + 394] = 3;
        if (s.origin == 'Q') put32(t0 + 1043, s.origin_id), put32(t0 + 1047, 0x20000005);
    } else {
        v.resize(v.size() + 1200, 0x11);
    }
    // Last Transform: position, rotation, scale (1, 1, 1), FF x 8.
    const size_t at = v.size();
    v.resize(at + 48, 0);
    putf(at, s.pos.x), putf(at + 4, s.pos.y), putf(at + 8, s.pos.z);
    putf(at + 12 + 4, 0.38f), putf(at + 12 + 12, 0.925f);
    putf(at + 28, 1.0f), putf(at + 32, 1.0f), putf(at + 36, 1.0f);
    std::memset(v.data() + at + 40, 0xFF, 8);
    v.resize(v.size() + 16, 0x22);
    auto push16 = [&](uint16_t x) { v.push_back(static_cast<uint8_t>(x)), v.push_back(static_cast<uint8_t>(x >> 8)); };
    auto push32 = [&](uint32_t x) { v.resize(v.size() + 4), std::memcpy(v.data() + v.size() - 4, &x, 4); };
    push16(static_cast<uint16_t>(s.hierarchy.size()));
    for (uint32_t k : s.hierarchy) push32(k);
    push32(s.parent);
    push16(static_cast<uint16_t>(s.children.size()));
    for (uint32_t k : s.children) push32(k);
    const uint16_t len = static_cast<uint16_t>(v.size() - 2);
    std::memcpy(v.data(), &len, 2);
    return v;
}

void test_enemy_zone_predicate() {
    using namespace enemy_load;
    // The table values these cases rely on.
    CHECK(is_table_hostile_faction(1000016) && is_table_hostile_faction(1000128) && !is_table_hostile_faction(1000001));
    CHECK(!is_excluded_variant(50031) && is_excluded_variant(54001));
    CHECK(origin_class(0x343A74A6) == 1 && origin_class(0xBF04A5ED) == 2 && origin_class(0x12345678) == 0);
    const ZoneRules plain{};
    LoadSpec bandit;
    {
        const auto load = make_load(bandit);
        const auto h = read_head(load);
        CHECK(h && h->type == 3 && h->key == 0xB0100223 && h->variant == 50031 && h->faction == 1000016);
        CHECK(name(load) == "Bleed Bandit");
        const auto t = last_transform(load);
        CHECK(t && t->pos.x == -10284.0f && t->pos.z == -3383.0f && t->rot.w == 0.925f);
        const auto o = origin(load);
        CHECK(o.kind == OriginKind::Schedule && o.id == 0x343A74A6 && o.sub == 7);
        const auto tail = parse_tail(load);
        CHECK(tail && tail->parent == 0 && tail->children.empty() && tail->hierarchy.empty());
        CHECK(zone_check(load, plain).verdict == Verdict::Yes);
    }
    auto verdict = [&](LoadSpec s, const ZoneRules& r = {}) { return zone_check(make_load(s), r).verdict; };
    {
        LoadSpec s = bandit;  // a camp leader: its members' tail names it
        s.children = {0xB0100224, 0xB0100225};
        const auto tail = parse_tail(make_load(s));
        CHECK(tail && tail->children.size() == 2 && tail->children[1] == 0xB0100225);
        CHECK(verdict(s) == Verdict::Yes);
    }
    {
        LoadSpec s = bandit;  // a member: its leader decides
        s.key = 0xB0100224;
        s.parent = 0xB0100223;
        const auto c = zone_check(make_load(s), plain);
        CHECK(c.verdict == Verdict::FollowParent && c.parent == 0xB0100223);
    }
    LoadSpec s = bandit;
    s.origin = 'F', s.origin_id = 1234;  // faction camp node
    CHECK(verdict(s) == Verdict::Yes);
    s.origin = '-';  // road bandit, no origin
    CHECK(verdict(s) == Verdict::Yes);
    s.origin = 'Q', s.origin_id = 0x343A74A6;  // an allowed sequence
    CHECK(verdict(s) == Verdict::Yes);
    s.origin = '?';  // T0 not found: not hidden (one too many is seen, never one too few)
    CHECK(verdict(s) == Verdict::No);
    s = bandit;
    s.origin_id = 0xBF04A5ED;  // a node event: only with the switch
    CHECK(verdict(s) == Verdict::No);
    CHECK(verdict(s, ZoneRules{true, {}}) == Verdict::Yes);
    s.origin_id = 0x12345678;  // anything else (quests, bosses, contents): never
    CHECK(verdict(s) == Verdict::No && verdict(s, ZoneRules{true, {}}) == Verdict::No);
    s = bandit;
    s.faction = 0;  // civilians and animals
    CHECK(verdict(s) == Verdict::No);
    s.faction = 1000001;  // not hostile by table...
    CHECK(verdict(s) == Verdict::No);
    const uint32_t learnt[] = {1000001};  // ...but learnt this session
    CHECK(verdict(s, ZoneRules{false, learnt}) == Verdict::Yes);
    s = bandit;
    s.variant = 54001;  // Boss_Myurdin
    CHECK(verdict(s) == Verdict::No);
    s = bandit;
    s.type = 6;  // an animal
    CHECK(verdict(s) == Verdict::No);
    s.type = 4;  // a named character
    CHECK(verdict(s) == Verdict::No);
    s = bandit;
    s.owner = 0x90100001;  // a mercenary or a pet
    CHECK(verdict(s) == Verdict::No);
    s = bandit;
    s.key = 0xB01F0001;  // one of our copies
    CHECK(verdict(s) == Verdict::No);
    s = bandit;
    s.hierarchy = {0xB0100300};
    CHECK(verdict(s) == Verdict::No);
    {
        auto load = make_load(bandit);
        load.push_back(0);  // not one record
        CHECK(zone_check(load, plain).verdict == Verdict::No);
        CHECK(zone_check(std::vector<uint8_t>(10, 0), plain).verdict == Verdict::No);
    }
    // The breaker's identity: same spawn = same identity, wherever the load stands.
    LoadSpec a = bandit, b = bandit;
    b.key = 0xB0200777;
    b.pos.x += 3.0f;
    CHECK(spawn_identity(make_load(a)) == spawn_identity(make_load(b)));
    b.origin_id = 0x58E673FD;
    CHECK(spawn_identity(make_load(a)) != spawn_identity(make_load(b)));
    CHECK(flat_distance({0, 0, 0}, {3, 100, 4}) == 5.0f);
}

void test_enemy_zone_message() {
    proto::EnemyZoneMsg req;
    req.flags = proto::EnemyZoneMsg::kOn | proto::EnemyZoneMsg::kDryRun;
    req.seq = 7;
    req.radius_m = 150.0f;
    auto bytes = proto::encode_enemy_zone(req);
    CHECK(bytes.size() == 9);
    auto d = proto::decode_enemy_zone(bytes);
    CHECK(d && d->on() && d->dry_run() && !d->answer() && !d->caution() && d->seq == 7 && d->radius_m == 150.0f);
    proto::EnemyZoneMsg ans;
    ans.flags = proto::EnemyZoneMsg::kAnswer | proto::EnemyZoneMsg::kOn | proto::EnemyZoneMsg::kCaution;
    ans.seq = 7;
    ans.radius_m = 120.0f;
    ans.factions = {1000001, 1000002};
    bytes = proto::encode_enemy_zone(ans);
    CHECK(bytes.size() == 9 + 2 + 8);
    d = proto::decode_enemy_zone(bytes);
    CHECK(d && d->answer() && d->on() && d->caution() && d->factions == ans.factions && d->radius_m == 120.0f);
    // Everything else is refused.
    auto bad = proto::encode_enemy_zone(req);
    bad[0] |= 0x10;  // unknown flag
    CHECK(!proto::decode_enemy_zone(bad));
    for (float r : {20.0f, 400.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        proto::EnemyZoneMsg m = req;
        m.radius_m = r;
        CHECK(!proto::decode_enemy_zone(proto::encode_enemy_zone(m)));
    }
    bad = proto::encode_enemy_zone(req);
    bad.push_back(0);  // a request carries no factions
    CHECK(!proto::decode_enemy_zone(bad));
    bad = proto::encode_enemy_zone(ans);
    bad.pop_back();  // truncated
    CHECK(!proto::decode_enemy_zone(bad));
    proto::EnemyZoneMsg zero = ans;
    zero.factions = {0};
    CHECK(!proto::decode_enemy_zone(proto::encode_enemy_zone(zero)));
    std::vector<uint8_t> many = {proto::EnemyZoneMsg::kAnswer, 1, 0, 0, 0};
    const float r = 150.0f;
    many.resize(many.size() + 4), std::memcpy(many.data() + 5, &r, 4);
    const auto n = static_cast<uint16_t>(proto::kMaxZoneFactions + 1);
    many.push_back(static_cast<uint8_t>(n)), many.push_back(static_cast<uint8_t>(n >> 8));
    for (uint16_t i = 0; i < n; ++i) many.insert(many.end(), {1, 0, 0, 0});
    CHECK(!proto::decode_enemy_zone(many));
    CHECK(!proto::decode_enemy_zone({}));
    // The session hands kind 8 to the game; a newer kind is dropped.
    static_assert(proto::kMaxRelKind == static_cast<uint8_t>(proto::RelKind::EnemyZone));
}

void test_enemy_zone_machine() {
    using Z = EnemyZoneMachine;
    const proto::Vec3 host{1000.0f, 50.0f, 1000.0f};
    auto at = [&](float d) { return proto::Vec3{host.x + d, host.y, host.z}; };
    auto in = [&](uint64_t now, float d) {
        Z::Input i;
        i.now_ms = now;
        i.linked = i.in_world = true;
        i.host = host;
        i.me = at(d);
        return i;
    };
    {
        Z z;
        CHECK(!z.tick(in(0, 40)).ask && z.state() == Z::State::Arming);
        CHECK(!z.tick(in(1900, 40)).ask);
        auto st = z.tick(in(2000, 40));
        CHECK(st.ask && z.seq() == 1 && z.state() == Z::State::Asking);
        // Nothing is hidden until the host answers that seq.
        CHECK(!z.tick(in(3000, 40)).turned_on && !z.on());
        CHECK(z.tick(in(12000, 40)).ask);  // asked again, same seq
        CHECK(z.seq() == 1);
        auto ok = in(12100, 40);
        ok.host_confirmed = true;
        CHECK(z.tick(ok).turned_on && z.on());
        // 150-250 m: stays on (hysteresis); beyond 250 m for 5 s: off, natives back.
        CHECK(!z.tick(in(12600, 100)).turned_off);
        CHECK(!z.tick(in(13500, 180)).turned_off);
        CHECK(!z.tick(in(14000, 260)).turned_off);
        CHECK(!z.tick(in(18900, 260)).turned_off);
        st = z.tick(in(19000, 260));
        CHECK(st.turned_off && st.restore && st.withdraw && st.why == Z::Why::Far && !z.on());
        // Back within 150 m: a new request with a new seq.
        z.tick(in(20100, 100));  // over a second later: not a jump
        CHECK(z.tick(in(22100, 100)).ask && z.seq() == 2);
    }
    auto turned_on = [&](Z& z, uint64_t& t) {
        z.tick(in(t, 30));
        t += 2000;
        z.tick(in(t, 30));
        auto ok = in(t += 100, 30);
        ok.host_confirmed = true;
        return z.tick(ok).turned_on;
    };
    {
        Z z;
        uint64_t t = 0;
        CHECK(turned_on(z, t));
        auto lost = in(t + 50, 30);
        lost.linked = false;  // link lost: at once, with our natives back
        auto st = z.tick(lost);
        CHECK(st.turned_off && st.restore && st.why == Z::Why::LinkLost);
    }
    {
        Z z;
        uint64_t t = 0;
        CHECK(turned_on(z, t));
        auto gone = in(t + 100, 30);
        gone.host.reset();  // the host's copy vanished: 3 s later
        CHECK(!z.tick(gone).turned_off);
        gone.now_ms = t + 3200;
        auto st = z.tick(gone);
        CHECK(st.turned_off && st.restore && st.why == Z::Why::HostGone);
    }
    {
        Z z;
        uint64_t t = 0;
        CHECK(turned_on(z, t));
        auto screen = in(t + 100, 30);
        screen.loading_screen = true;  // the server removed everything: nothing to give back
        auto st = z.tick(screen);
        CHECK(st.turned_off && !st.restore && st.why == Z::Why::LoadingScreen);
    }
    {
        Z z;
        uint64_t t = 0;
        CHECK(turned_on(z, t));
        // One bad read 500 m away is not a jump...
        auto odd = in(t + 100, 30);
        odd.me = at(500);
        CHECK(!z.tick(odd).turned_off);
        CHECK(!z.tick(in(t + 150, 30)).turned_off && z.on());
        // ...staying there is (we fast-travelled: the server removes our natives itself).
        auto far = in(t + 200, 30);
        far.me = at(800);
        CHECK(!z.tick(far).turned_off);
        far.now_ms = t + 600;
        auto st = z.tick(far);
        CHECK(st.turned_off && !st.restore && st.why == Z::Why::YouJumped);
    }
    {
        Z z;
        uint64_t t = 0;
        CHECK(turned_on(z, t));
        auto moved = in(t + 100, 30);
        moved.host = proto::Vec3{host.x + 400, host.y, host.z};  // the host fast-travelled
        moved.me = at(30);
        z.tick(moved);
        moved.now_ms = t + 500;
        auto st = z.tick(moved);
        CHECK(st.turned_off && st.restore && st.why == Z::Why::HostJumped);
    }
    // The host jumped but stays within off_m: the zone is kept.
    {
        Z z;
        uint64_t t = 0;
        CHECK(turned_on(z, t));
        auto moved = in(t + 100, 30);
        moved.host = proto::Vec3{host.x + 200, host.y, host.z};
        moved.me = at(30);
        z.tick(moved);
        moved.now_ms = t + 500;
        auto st = z.tick(moved);
        CHECK(!st.turned_off && !st.paused && z.on());
    }
    // The host died and reappeared 400 m away (zona-tras-muerte-anfitrion.md):
    // kept for the grace, then Far with the give-back; back within off_m: resumed.
    auto respawned = [&](Z& z, uint64_t& t) {
        CHECK(turned_on(z, t));
        auto dead = in(t += 100, 30);
        dead.host_dead = true;
        z.tick(dead);
        dead.now_ms = t += 5000;
        z.tick(dead);
        auto moved = in(t += 100, 30);
        moved.host = proto::Vec3{host.x + 400, host.y, host.z};
        moved.me = at(30);
        z.tick(moved);
        moved.now_ms = t += 400;
        return z.tick(moved);
    };
    {
        Z z;
        uint64_t t = 0;
        auto st = respawned(z, t);
        CHECK(st.paused && !st.turned_off && z.on() && z.grace_until() == t + 60000);
        auto far = in(t, 30);
        far.host = proto::Vec3{host.x + 400, host.y, host.z};
        far.me = at(30);
        far.now_ms = t + 59000;
        CHECK(!z.tick(far).turned_off);
        far.host.reset();  // its copy gone a while: no HostGone during the grace
        far.now_ms = t + 59500;
        CHECK(!z.tick(far).turned_off);
        far.host = proto::Vec3{host.x + 400, host.y, host.z};
        far.now_ms = t + 60000;
        st = z.tick(far);
        CHECK(st.turned_off && st.restore && st.why == Z::Why::Far);
    }
    {
        Z z;
        uint64_t t = 0;
        CHECK(respawned(z, t).paused);
        auto back = in(t + 20000, 30);  // the host came back
        back.host = proto::Vec3{host.x + 100, host.y, host.z};
        back.me = at(30);
        auto st = z.tick(back);
        CHECK(st.resumed && z.on() && z.grace_until() == 0);
        // Then away for good (not a jump: over a second): the usual 5 s.
        back.host = proto::Vec3{host.x + 300, host.y, host.z};
        back.now_ms = t + 21200;
        CHECK(!z.tick(back).turned_off);
        back.now_ms = t + 26300;
        st = z.tick(back);
        CHECK(st.turned_off && st.why == Z::Why::Far);
    }
    {
        // During the grace a loading screen still ends it, with nothing given back.
        Z z;
        uint64_t t = 0;
        CHECK(respawned(z, t).paused);
        auto screen = in(t + 1000, 30);
        screen.host = proto::Vec3{host.x + 400, host.y, host.z};
        screen.me = at(30);
        screen.loading_screen = true;
        auto st = z.tick(screen);
        CHECK(st.turned_off && !st.restore && st.why == Z::Why::LoadingScreen);
    }
    {
        // Fighting our own natives: the request waits for 10 s of calm, 60 s at most.
        Z z;
        auto i = in(0, 30);
        i.last_fight_ms = 1;
        z.tick(i);
        i.now_ms = 2000;
        i.last_fight_ms = 1900;
        CHECK(!z.tick(i).ask && z.state() == Z::State::WaitFight);
        i.now_ms = 11800;
        CHECK(!z.tick(i).ask);
        i.now_ms = 11950;
        CHECK(z.tick(i).ask);
        Z busy;
        auto b = in(0, 30);
        busy.tick(b);
        b.now_ms = 2000;
        b.last_fight_ms = 2000;
        busy.tick(b);
        b.now_ms = 61000;
        b.last_fight_ms = 61000;
        CHECK(!busy.tick(b).ask);
        b.now_ms = 62000;
        b.last_fight_ms = 62000;
        CHECK(busy.tick(b).ask);
    }
    {
        // A host that does not relay: off, and not asked again for 30 s.
        Z z;
        z.tick(in(0, 30));
        CHECK(z.tick(in(2000, 30)).ask);
        auto no = in(2100, 30);
        no.host_refused = true;
        auto st = z.tick(no);
        CHECK(st.withdraw && !st.turned_off && st.why == Z::Why::HostStopped && z.state() == Z::State::Off);
        z.tick(in(3000, 30));
        CHECK(z.state() == Z::State::Off);
        z.tick(in(32200, 30));
        CHECK(z.state() == Z::State::Arming);
    }
    {
        // Stopped from outside (breaker, user).
        Z z;
        uint64_t t = 0;
        CHECK(turned_on(z, t));
        auto st = z.stop(Z::Why::Stopped, true, t + 10);
        CHECK(st.turned_off && st.restore && st.withdraw && !z.on());
        CHECK(!z.stop(Z::Why::Stopped, true, t + 20).turned_off);
    }
    {
        SpawnBreaker b;
        for (int i = 0; i < 4; ++i) CHECK(!b.note(42, 1000 + i * 1000));
        CHECK(b.note(42, 6000));  // a fifth time in 2 min
        SpawnBreaker slow;
        for (int i = 0; i < 10; ++i) CHECK(!slow.note(42, static_cast<uint64_t>(i) * 31000));
        SpawnBreaker many;
        bool tripped = false;
        for (uint64_t i = 0; i < 60; ++i) tripped = tripped || many.note(i, 1000 + i * 500);
        CHECK(!tripped);
        CHECK(many.note(999, 31000));  // 61 in a minute
    }
}

}  // namespace

int main() {
    test_pattern();
    test_byte_ring();
    test_math();
    test_roundtrip();
    test_rejects_bad_input();
    test_session_handshake_and_retry();
    test_session_ping_rtt_and_timeout();
    test_session_bye_and_garbage();
    test_coop_messages();
    test_reliable_channel();
    test_reliable_order_and_streams();
    test_reliable_budget();
    test_versions();
    test_session_game_data();
    test_clock_mapper();
    test_equip_state();
    test_inject_clock();
    test_field_time();
    test_learnt_file();
    test_session_host_guest();
    test_enemy_zone_predicate();
    test_enemy_zone_message();
    test_enemy_zone_machine();
    if (g_failures == 0) std::printf("All tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
