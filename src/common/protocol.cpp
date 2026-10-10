#include "common/protocol.h"

#include <cmath>
#include <cstring>

namespace cdmp::proto {
namespace {

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { raw(&v, sizeof v); }
    void u32(uint32_t v) { raw(&v, sizeof v); }
    void f32(float v) { raw(&v, sizeof v); }
    void bytes(std::span<const uint8_t> b) { raw(b.data(), b.size()); }
    void str(const std::string& s) {
        const size_t n = s.size() < kMaxNameLen ? s.size() : kMaxNameLen;
        u8(static_cast<uint8_t>(n));
        raw(s.data(), n);
    }
    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    void raw(const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    uint8_t u8() {
        uint8_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    uint16_t u16() {
        uint16_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    float f32() {
        float v = 0;
        raw(&v, sizeof v);
        if (!std::isfinite(v)) ok_ = false;
        return v;
    }
    std::vector<uint8_t> bytes(size_t n) {
        if (!has(n)) {
            ok_ = false;
            return {};
        }
        std::vector<uint8_t> v(bytes_.begin() + static_cast<ptrdiff_t>(pos_),
                               bytes_.begin() + static_cast<ptrdiff_t>(pos_ + n));
        pos_ += n;
        return v;
    }
    bool at_end() const { return pos_ == bytes_.size(); }
    std::string str() {
        const uint8_t n = u8();
        if (n > kMaxNameLen || !has(n)) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(bytes_.data() + pos_), n);
        pos_ += n;
        // Names end up in log files and UI; keep them printable ASCII.
        for (char& c : s) {
            if (c < 0x20 || c > 0x7E) c = '?';
        }
        return s;
    }
    bool ok() const { return ok_; }

private:
    bool has(size_t n) const { return ok_ && bytes_.size() - pos_ >= n; }
    void raw(void* out, size_t n) {
        if (!has(n)) {
            ok_ = false;
            return;
        }
        std::memcpy(out, bytes_.data() + pos_, n);
        pos_ += n;
    }

    std::span<const uint8_t> bytes_;
    size_t pos_ = 0;
    bool ok_ = true;
};

void write_vec3(Writer& w, const Vec3& v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
}
Vec3 read_vec3(Reader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}
void write_quat(Writer& w, const Quat& q) {
    w.f32(q.x);
    w.f32(q.y);
    w.f32(q.z);
    w.f32(q.w);
}
Quat read_quat(Reader& r) {
    Quat q;
    q.x = r.f32();
    q.y = r.f32();
    q.z = r.f32();
    q.w = r.f32();
    return q;
}

}  // namespace

Type type_of(const Body& body) {
    struct Visitor {
        Type operator()(const Hello&) const { return Type::Hello; }
        Type operator()(const Welcome&) const { return Type::Welcome; }
        Type operator()(const Bye&) const { return Type::Bye; }
        Type operator()(const Ping&) const { return Type::Ping; }
        Type operator()(const Pong&) const { return Type::Pong; }
        Type operator()(const PlayerState&) const { return Type::PlayerState; }
        Type operator()(const GameEvent&) const { return Type::GameEvent; }
        Type operator()(const RelData&) const { return Type::RelData; }
        Type operator()(const RelAck&) const { return Type::RelAck; }
    };
    return std::visit(Visitor{}, body);
}

const char* type_name(Type type) {
    switch (type) {
        case Type::Hello: return "Hello";
        case Type::Welcome: return "Welcome";
        case Type::Bye: return "Bye";
        case Type::Ping: return "Ping";
        case Type::Pong: return "Pong";
        case Type::PlayerState: return "PlayerState";
        case Type::GameEvent: return "GameEvent";
        case Type::RelData: return "RelData";
        case Type::RelAck: return "RelAck";
    }
    return "Unknown";
}

std::vector<uint8_t> encode(const Packet& packet) {
    Writer w;
    w.u32(kMagic);
    w.u16(kVersion);
    w.u8(static_cast<uint8_t>(type_of(packet.body)));
    w.u8(0);
    w.u32(packet.seq);
    w.u32(packet.send_ms);

    struct BodyWriter {
        Writer& w;
        void operator()(const Hello& b) const {
            w.str(b.name);
            w.u32(b.mod_build);
            w.u32(b.nonce);
        }
        void operator()(const Welcome& b) const {
            w.str(b.name);
            w.u32(b.nonce);
            w.u32(b.mod_build);
        }
        void operator()(const Bye&) const {}
        void operator()(const Ping& b) const { w.u32(b.origin_ms); }
        void operator()(const Pong& b) const { w.u32(b.origin_ms); }
        void operator()(const PlayerState& b) const {
            write_vec3(w, b.pos);
            write_quat(w, b.rot);
            w.f32(b.health);
            w.f32(b.health_max);
            w.u8(b.flags);
        }
        void operator()(const GameEvent& b) const {
            w.u8(static_cast<uint8_t>(b.kind));
            w.u16(static_cast<uint16_t>(b.payload.size()));
            w.bytes(b.payload);
        }
        void operator()(const RelData& b) const {
            w.u32(b.stream);
            w.u32(b.msg_id);
            w.u16(b.index);
            w.u16(b.count);
            w.u8(static_cast<uint8_t>(b.kind));
            w.u16(static_cast<uint16_t>(b.chunk.size()));
            w.bytes(b.chunk);
        }
        void operator()(const RelAck& b) const {
            w.u32(b.msg_id);
            w.u16(b.index);
        }
    };
    std::visit(BodyWriter{w}, packet.body);
    return w.take();
}

std::optional<Packet> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || bytes.size() > kMaxPacketSize) return std::nullopt;

    Reader r(bytes);
    if (r.u32() != kMagic) return std::nullopt;
    const uint16_t version = r.u16();
    const auto type = static_cast<Type>(r.u8());
    r.u8();  // reserved

    Packet p;
    p.version = version;
    p.seq = r.u32();
    p.send_ms = r.u32();
    if (version != kVersion) {
        // Another version: only who it is, to tell the player (protocol.h).
        if (version < 5 || (type != Type::Hello && type != Type::Welcome)) return std::nullopt;
        std::string name = r.str();
        if (!r.ok()) return std::nullopt;
        if (type == Type::Hello) p.body = Hello{std::move(name)};
        else p.body = Welcome{std::move(name)};
        return p;
    }

    switch (type) {
        case Type::Hello: {
            Hello b;
            b.name = r.str();
            b.mod_build = r.u32();
            b.nonce = r.u32();
            p.body = b;
            break;
        }
        case Type::Welcome: {
            Welcome b;
            b.name = r.str();
            b.nonce = r.u32();
            b.mod_build = r.u32();
            p.body = b;
            break;
        }
        case Type::Bye:
            p.body = Bye{};
            break;
        case Type::Ping:
            p.body = Ping{r.u32()};
            break;
        case Type::Pong:
            p.body = Pong{r.u32()};
            break;
        case Type::PlayerState: {
            PlayerState b;
            b.pos = read_vec3(r);
            b.rot = read_quat(r);
            b.health = r.f32();
            b.health_max = r.f32();
            b.flags = r.u8();
            // Only what a real player can be: inside the world, a rotation,
            // a plausible health (security review B3).
            const float q2 = b.rot.x * b.rot.x + b.rot.y * b.rot.y + b.rot.z * b.rot.z + b.rot.w * b.rot.w;
            if (std::fabs(b.pos.x) > kMaxWorldCoord || std::fabs(b.pos.z) > kMaxWorldCoord ||
                std::fabs(b.pos.y) > kMaxWorldHeight || q2 < 0.5f || q2 > 1.5f || b.health < 0 ||
                b.health > 1e7f || b.health_max < 0 || b.health_max > 1e7f) {
                return std::nullopt;
            }
            p.body = b;
            break;
        }
        case Type::GameEvent: {
            GameEvent b;
            const uint8_t kind = r.u8();
            if (kind == 0) return std::nullopt;
            b.kind = static_cast<EventKind>(kind);
            const uint16_t n = r.u16();
            if (n > kMaxEventPayload) return std::nullopt;
            b.payload = r.bytes(n);
            p.body = std::move(b);
            break;
        }
        case Type::RelData: {
            RelData b;
            b.stream = r.u32();
            b.msg_id = r.u32();
            b.index = r.u16();
            b.count = r.u16();
            const uint8_t kind = r.u8();
            if (kind == 0) return std::nullopt;
            b.kind = static_cast<RelKind>(kind);
            const uint16_t n = r.u16();
            if (n > kRelChunk || b.count == 0 || b.index >= b.count ||
                b.count > (kMaxRelMessage + kRelChunk - 1) / kRelChunk) {
                return std::nullopt;
            }
            b.chunk = r.bytes(n);
            p.body = std::move(b);
            break;
        }
        case Type::RelAck: {
            RelAck b;
            b.msg_id = r.u32();
            b.index = r.u16();
            p.body = b;
            break;
        }
        default:
            return std::nullopt;
    }

    if (!r.ok() || !r.at_end()) return std::nullopt;
    return p;
}

std::vector<uint8_t> encode_spawn(const SpawnInfo& info) {
    Writer w;
    w.u32(info.generation);
    w.u32(info.user_key);
    w.u32(info.player_key);
    w.u32(info.body_key);
    write_vec3(w, info.pos);
    write_quat(w, info.rot);
    w.u32(static_cast<uint32_t>(info.user.size()));
    w.bytes(info.user);
    w.u32(static_cast<uint32_t>(info.body.size()));
    w.bytes(info.body);
    w.u32(static_cast<uint32_t>(info.equip.size()));
    w.bytes(info.equip);
    w.u32(info.horse_key);
    write_vec3(w, info.horse_pos);
    w.u32(static_cast<uint32_t>(info.horse.size()));
    w.bytes(info.horse);
    return w.take();
}

std::optional<SpawnInfo> decode_spawn(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    SpawnInfo info;
    info.generation = r.u32();
    info.user_key = r.u32();
    info.player_key = r.u32();
    info.body_key = r.u32();
    info.pos = read_vec3(r);
    info.rot = read_quat(r);
    const uint32_t nu = r.u32();
    if (nu > 0xFFFF) return std::nullopt;  // game packets carry a u16 length
    info.user = r.bytes(nu);
    const uint32_t nb = r.u32();
    if (nb > 0xFFFF) return std::nullopt;
    info.body = r.bytes(nb);
    const uint32_t ne = r.u32();
    if (ne > 0xFFFF) return std::nullopt;
    info.equip = r.bytes(ne);
    info.horse_key = r.u32();
    info.horse_pos = read_vec3(r);
    const uint32_t nh = r.u32();
    if (nh > 0xFFFF) return std::nullopt;
    info.horse = r.bytes(nh);
    // Loads carry a Transform 154 bytes before their end; anything shorter is not one.
    if (!r.ok() || !r.at_end() || info.user.size() < 256 || info.body.size() < 256 ||
        (!info.horse.empty() && info.horse.size() < 256)) {
        return std::nullopt;
    }
    return info;
}

std::vector<uint8_t> encode_enemy_zone(const EnemyZoneMsg& msg) {
    Writer w;
    w.u8(msg.flags);
    w.u32(msg.seq);
    w.f32(msg.radius_m);
    if (msg.answer()) {
        const size_t n = msg.factions.size() < kMaxZoneFactions ? msg.factions.size() : kMaxZoneFactions;
        w.u16(static_cast<uint16_t>(n));
        for (size_t i = 0; i < n; ++i) w.u32(msg.factions[i]);
    }
    return w.take();
}

std::optional<EnemyZoneMsg> decode_enemy_zone(std::span<const uint8_t> bytes) {
    constexpr uint8_t kKnown = EnemyZoneMsg::kOn | EnemyZoneMsg::kDryRun | EnemyZoneMsg::kCaution |
                               EnemyZoneMsg::kAnswer;
    Reader r(bytes);
    EnemyZoneMsg m;
    m.flags = r.u8();
    m.seq = r.u32();
    m.radius_m = r.f32();
    if (!r.ok() || (m.flags & ~kKnown) || !(m.radius_m >= kMinZoneRadius && m.radius_m <= kMaxZoneRadius)) {
        return std::nullopt;
    }
    if (m.answer()) {
        const uint16_t n = r.u16();
        if (n > kMaxZoneFactions) return std::nullopt;
        m.factions.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            const uint32_t f = r.u32();
            if (f == 0) return std::nullopt;
            m.factions.push_back(f);
        }
    }
    if (!r.ok() || !r.at_end()) return std::nullopt;
    return m;
}

}  // namespace cdmp::proto
