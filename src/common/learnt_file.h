#pragma once
// What the mod learnt about combat in this player's own game, kept between
// sessions in cdmp_learnt.txt next to cdmp.asi, so a player who also plays
// alone brings their own numbers to the next co-op session:
// - their attacks: HP on a defence-1.0 enemy (their gear), how often they
//   stagger and whether they ever hurt (guest_hits.cpp);
// - each enemy type's defence;
// - what each enemy attack takes from them and how often it staggers them
//   (player_hits.cpp).
// Numbers only: the packet templates name actors of one session and are
// learnt again in each one. The file is plain text, one fact per line; a
// line that does not parse or is out of range is skipped, and a file of
// another game build is ignored (attack hashes may change with a patch).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cdmp {

struct LearntCombat {
    struct Attack {  // one of our attacks: accessor action hash << 32 | frame
        std::map<int, int> base_votes;  // base HP x10 -> clean blows that gave it
        uint32_t clean = 0, no_damage = 0, staggers = 0, blows = 0;
    };
    struct Reaction {  // an enemy attack on us: how often it staggered us
        uint32_t blows = 0, staggers = 0;
    };
    std::map<uint64_t, Attack> attacks;
    std::map<uint32_t, double> defence;       // enemy variant -> defence factor
    std::map<uint64_t, int64_t> enemy_hits;   // variant << 32 | attack hash -> HP x1000 it took from us
    std::map<uint32_t, int64_t> enemy_types;  // variant -> HP x1000 of its latest blow on us
    std::map<uint64_t, Reaction> reactions;   // variant << 32 | attack hash

    bool empty() const {
        return attacks.empty() && defence.empty() && enemy_hits.empty() && enemy_types.empty() && reactions.empty();
    }
};

namespace learnt {

constexpr std::string_view kHeader = "cdmp-learnt 1";
constexpr size_t kMaxFileBytes = 1 << 20;
constexpr size_t kMaxEntries = 4096;  // per kind
// What a new session starts from: each attack's base keeps at most this many
// votes, so new gear outvotes the old base after a few clean blows, and its
// counters are scaled down to this many blows (the ratios stay).
constexpr int kKeptVotes = 3;
constexpr uint32_t kKeptBlows = 20;
// Ranges of real values (pieza4-capturas.md: bases 5-56 HP, defences
// 0.49-1.01; enemy blows up to a few hundred HP).
constexpr int kMaxBaseX10 = 5000;
constexpr double kMinDefence = 0.05, kMaxDefence = 5.0;
constexpr int64_t kMaxEnemyHit = 10'000'000;  // 10,000 HP
constexpr uint32_t kMaxCount = 1'000'000;

inline uint32_t scaled(uint32_t v, uint32_t blows) {
    return blows > kKeptBlows ? static_cast<uint32_t>((static_cast<uint64_t>(v) * kKeptBlows + blows / 2) / blows) : v;
}

inline std::string format(const LearntCombat& c, uint32_t game_build) {
    std::string out;
    char line[160];
    out += kHeader;
    out += "\n# Crimson Desert Multiplayer: combat values learnt in this game (delete the file to start over)\n";
    std::snprintf(line, sizeof line, "build %08X\n", game_build);
    out += line;
    for (const auto& [act, a] : c.attacks) {
        if (a.base_votes.empty() && !a.blows) continue;
        int base = 0, votes = 0;
        for (const auto& [b, n] : a.base_votes) {
            if (n > votes) base = b, votes = n;
        }
        std::snprintf(line, sizeof line, "attack %08X %u %d %d %u %u %u %u\n", static_cast<uint32_t>(act >> 32),
                      static_cast<uint32_t>(act), base, std::min(votes, kKeptVotes), scaled(a.clean, a.blows),
                      scaled(a.no_damage, a.blows), scaled(a.staggers, a.blows), std::min(a.blows, kKeptBlows));
        out += line;
    }
    for (const auto& [variant, f] : c.defence) {
        std::snprintf(line, sizeof line, "defence %u %.4f\n", variant, f);
        out += line;
    }
    for (const auto& [key, hp] : c.enemy_hits) {
        std::snprintf(line, sizeof line, "enemyhit %u %08X %lld\n", static_cast<uint32_t>(key >> 32),
                      static_cast<uint32_t>(key), static_cast<long long>(hp));
        out += line;
    }
    for (const auto& [variant, hp] : c.enemy_types) {
        std::snprintf(line, sizeof line, "enemytype %u %lld\n", variant, static_cast<long long>(hp));
        out += line;
    }
    for (const auto& [key, r] : c.reactions) {
        if (!r.blows) continue;
        std::snprintf(line, sizeof line, "reaction %u %08X %u %u\n", static_cast<uint32_t>(key >> 32),
                      static_cast<uint32_t>(key), std::min(r.blows, kKeptBlows), scaled(r.staggers, r.blows));
        out += line;
    }
    return out;
}

// Splits on spaces; numbers in base 10 except the hex fields the caller asks for.
inline std::vector<std::string_view> fields(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size() && out.size() < 16) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}
template <typename T>
bool num(std::string_view s, T& v, int base = 10) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
inline bool real(std::string_view s, double& v) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size() && std::isfinite(v);
}

// nullopt: not our file, or learnt in another game build.
inline std::optional<LearntCombat> parse(std::string_view text, uint32_t game_build) {
    if (text.size() > kMaxFileBytes) return std::nullopt;
    LearntCombat c;
    bool header = false, build = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!header) {
            if (line != kHeader) return std::nullopt;
            header = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        const auto f = fields(line);
        if (f.empty()) continue;
        if (!build) {
            uint32_t b = 0;
            if (f.size() != 2 || f[0] != "build" || !num(f[1], b, 16) || b != game_build) return std::nullopt;
            build = true;
            continue;
        }
        if (f[0] == "attack" && f.size() == 9 && c.attacks.size() < kMaxEntries) {
            uint32_t hash = 0, frame = 0, clean = 0, no_damage = 0, staggers = 0, blows = 0;
            int base = 0, votes = 0;
            if (!num(f[1], hash, 16) || !num(f[2], frame) || !num(f[3], base) || !num(f[4], votes) ||
                !num(f[5], clean) || !num(f[6], no_damage) || !num(f[7], staggers) || !num(f[8], blows)) {
                continue;
            }
            if (base < 0 || base > kMaxBaseX10 || votes < 0 || votes > kKeptVotes || blows > kMaxCount ||
                clean > blows || no_damage > blows || staggers > blows || (base > 0) != (votes > 0)) {
                continue;
            }
            LearntCombat::Attack a;
            if (base > 0) a.base_votes[base] = votes;
            a.clean = clean, a.no_damage = no_damage, a.staggers = staggers, a.blows = blows;
            c.attacks[static_cast<uint64_t>(hash) << 32 | frame] = std::move(a);
        } else if (f[0] == "defence" && f.size() == 3 && c.defence.size() < kMaxEntries) {
            uint32_t variant = 0;
            double factor = 0;
            if (num(f[1], variant) && variant && real(f[2], factor) && factor >= kMinDefence && factor <= kMaxDefence) {
                c.defence[variant] = factor;
            }
        } else if (f[0] == "enemyhit" && f.size() == 4 && c.enemy_hits.size() < kMaxEntries) {
            uint32_t variant = 0, hash = 0;
            int64_t hp = 0;
            if (num(f[1], variant) && variant && num(f[2], hash, 16) && num(f[3], hp) && hp > 0 && hp <= kMaxEnemyHit) {
                c.enemy_hits[static_cast<uint64_t>(variant) << 32 | hash] = hp;
            }
        } else if (f[0] == "enemytype" && f.size() == 3 && c.enemy_types.size() < kMaxEntries) {
            uint32_t variant = 0;
            int64_t hp = 0;
            if (num(f[1], variant) && variant && num(f[2], hp) && hp > 0 && hp <= kMaxEnemyHit) {
                c.enemy_types[variant] = hp;
            }
        } else if (f[0] == "reaction" && f.size() == 5 && c.reactions.size() < kMaxEntries) {
            uint32_t variant = 0, hash = 0, blows = 0, staggers = 0;
            if (num(f[1], variant) && variant && num(f[2], hash, 16) && num(f[3], blows) && num(f[4], staggers) &&
                blows > 0 && blows <= kMaxCount && staggers <= blows) {
                c.reactions[static_cast<uint64_t>(variant) << 32 | hash] = {blows, staggers};
            }
        }
    }
    if (!header || !build) return std::nullopt;
    return c;
}

}  // namespace learnt
}  // namespace cdmp
