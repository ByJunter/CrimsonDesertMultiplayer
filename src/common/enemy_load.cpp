#include "common/enemy_load.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "common/enemy_tables_2976.h"
#include "common/keys.h"

namespace cdmp::enemy_load {
namespace {

using net::keys::is_class;

uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool is_key_shape(uint32_t v) { return is_class(v, net::keys::kClassWorld) || is_class(v, net::keys::kClassPlayer); }

template <size_t N>
bool sorted_has(const uint32_t (&table)[N], uint32_t v) {
    return std::binary_search(std::begin(table), std::end(table), v);
}

// 0C 00 | 8 x 00 | FF x 4, then 4 bytes, then T0 (unique in 5,332 of 5,332
// captured loads; the same search as enemy_relay's strip_spawn_identity).
std::optional<size_t> find_t0(std::span<const uint8_t> load) {
    static const uint8_t kSig[14] = {0x0C, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    for (size_t i = 0x24; i + sizeof kSig + 18 + 40 <= load.size(); ++i) {
        if (std::memcmp(load.data() + i, kSig, sizeof kSig) == 0) return i + 18;
    }
    return std::nullopt;
}

}  // namespace

std::optional<Head> read_head(std::span<const uint8_t> load) {
    if (load.size() < 0x40) return std::nullopt;
    uint16_t len;
    std::memcpy(&len, load.data(), 2);
    if (static_cast<size_t>(len) + 2 != load.size()) return std::nullopt;
    Head h;
    h.type = load[2];
    h.key = u32_at(load.data() + 3);
    h.owner = u32_at(load.data() + 7);
    h.variant = u32_at(load.data() + 0x18);
    h.faction = u32_at(load.data() + 0x1C);
    return h;
}

std::string name(std::span<const uint8_t> load) {
    std::string s;
    if (load.size() <= 0x24) return s;
    const uint32_t chars = u32_at(load.data() + 0x20);
    for (uint32_t i = 0; i < chars && i < 40 && 0x24 + 2 * static_cast<size_t>(i) + 1 < load.size(); ++i) {
        const char c = static_cast<char>(load[0x24 + 2 * static_cast<size_t>(i)]);
        s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return s;
}

std::optional<Transform> last_transform(std::span<const uint8_t> load) {
    static const uint8_t kMarker[20] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
                                        0x80, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (load.size() < sizeof kMarker + 29) return std::nullopt;
    for (size_t i = load.size() - sizeof kMarker; i > 28; --i) {
        if (std::memcmp(load.data() + i, kMarker, sizeof kMarker) != 0) continue;
        float f[7];
        std::memcpy(f, load.data() + i - 28, sizeof f);
        for (float v : f) {
            if (!std::isfinite(v)) return std::nullopt;
        }
        return Transform{i - 28, {f[0], f[1], f[2]}, {f[3], f[4], f[5], f[6]}};
    }
    return std::nullopt;
}

std::optional<Tail> parse_tail(std::span<const uint8_t> pl) {
    const size_t len = pl.size();
    for (size_t n = 0; n <= 64; ++n) {
        if (len < 2 + 4 * n + 8) break;
        const size_t o = len - 2 - 4 * n;
        if (o < 8) break;
        uint16_t cnt;
        std::memcpy(&cnt, pl.data() + o, 2);
        if (cnt != n) continue;
        bool keys = true;
        for (size_t i = 0; i < n; ++i) keys = keys && is_key_shape(u32_at(pl.data() + o + 2 + 4 * i));
        if (!keys) continue;
        const size_t ac = o - 4;
        for (size_t m = 0; m <= 32; ++m) {
            if (ac < 4 * m + 2 + 8) break;
            const size_t ho = ac - 4 * m - 2;
            uint16_t hm;
            std::memcpy(&hm, pl.data() + ho, 2);
            if (hm != m) continue;
            bool hk = true;
            for (size_t i = 0; i < m; ++i) hk = hk && is_key_shape(u32_at(pl.data() + ho + 2 + 4 * i));
            if (!hk) continue;
            Tail t;
            t.ac = ac;
            t.parent = u32_at(pl.data() + ac);
            for (size_t i = 0; i < n; ++i) t.children.push_back(u32_at(pl.data() + o + 2 + 4 * i));
            for (size_t i = 0; i < m; ++i) t.hierarchy.push_back(u32_at(pl.data() + ho + 2 + 4 * i));
            return t;
        }
    }
    return std::nullopt;
}

Origin origin(std::span<const uint8_t> load) {
    const auto t0 = find_t0(load);
    if (!t0) return {};
    auto at = [&](size_t off) { return *t0 + off + 4 <= load.size() ? u32_at(load.data() + *t0 + off) : 0u; };
    if (const uint32_t schedule = at(42)) return {OriginKind::Schedule, schedule, at(46)};
    if (const uint32_t seq = at(1043); seq && (at(1047) & 0xF0000000u) == 0x20000000u) {
        return {OriginKind::Sequence, seq, at(1047) & 0xFFFFu};
    }
    if (const uint32_t node = at(390); node && *t0 + 396 <= load.size()) {
        uint16_t slot;
        std::memcpy(&slot, load.data() + *t0 + 394, 2);
        return {OriginKind::FactionNode, node, slot};
    }
    return {OriginKind::None, 0, 0};
}

float flat_distance(const proto::Vec3& a, const proto::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

bool is_table_hostile_faction(uint32_t faction) { return sorted_has(tables_2976::kHostileFactions, faction); }
bool is_excluded_variant(uint32_t variant) { return sorted_has(tables_2976::kExcludedVariants, variant); }
int origin_class(uint32_t hash) {
    if (sorted_has(tables_2976::kAllowedOrigins, hash)) return 1;
    if (sorted_has(tables_2976::kCautionOrigins, hash)) return 2;
    return 0;
}

Check zone_check(std::span<const uint8_t> load, const ZoneRules& rules) {
    const auto h = read_head(load);
    if (!h) return {Verdict::No, 0, "not one record"};
    if (h->type != 3) return {Verdict::No, 0, "not a person"};
    if (h->owner != 0) return {Verdict::No, 0, "it has an owner"};
    if (!net::keys::is_game_world_key(h->key)) return {Verdict::No, 0, "not a world key of the game"};
    const auto tail = parse_tail(load);
    if (!tail) return {Verdict::No, 0, "its tail could not be read"};
    if (!tail->hierarchy.empty()) return {Verdict::No, 0, "it has hierarchy children"};
    const bool learnt = std::find(rules.learnt_factions.begin(), rules.learnt_factions.end(), h->faction) !=
                        rules.learnt_factions.end();
    if (h->faction == 0 || (!is_table_hostile_faction(h->faction) && !learnt)) {
        return {Verdict::No, 0, "not a hostile faction"};
    }
    if (is_excluded_variant(h->variant)) return {Verdict::No, 0, "a boss or unique variant"};
    const Origin o = origin(load);
    switch (o.kind) {
        case OriginKind::Unknown: return {Verdict::No, 0, "no T0"};
        case OriginKind::Schedule:
        case OriginKind::Sequence: {
            const int c = origin_class(o.id);
            if (c == 0) return {Verdict::No, 0, "origin not allowed"};
            if (c == 2 && !rules.caution) return {Verdict::No, 0, "node or faction event"};
            break;
        }
        case OriginKind::FactionNode:
        case OriginKind::None: break;
    }
    if (tail->parent) return {Verdict::FollowParent, tail->parent, "a camp member"};
    return {Verdict::Yes, 0, ""};
}

uint64_t spawn_identity(std::span<const uint8_t> load) {
    const auto h = read_head(load);
    const uint64_t variant = h ? h->variant : 0;
    const Origin o = origin(load);
    uint64_t where = static_cast<uint64_t>(o.id) * 0x9E3779B1u + o.sub;
    if (o.kind == OriginKind::None || o.kind == OriginKind::Unknown) {
        if (const auto t = last_transform(load)) {
            const auto cx = static_cast<int32_t>(std::floor(t->pos.x / 10.0f));
            const auto cz = static_cast<int32_t>(std::floor(t->pos.z / 10.0f));
            where = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cz);
        }
    }
    return (variant << 40) ^ (static_cast<uint64_t>(static_cast<uint8_t>(o.kind)) << 32) ^ where;
}

}  // namespace cdmp::enemy_load
