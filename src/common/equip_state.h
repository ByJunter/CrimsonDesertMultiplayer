#pragma once
// The gear a player changed since their character templates were captured,
// in the form the other PC must replay it on a freshly loaded copy (whose
// gear is the template's).
//
// OtherEquipItemAck payload: 04 | u32 key | u32 serial | n x {u32 item,
// u16 slot}; item 0 = remove. The server always sends a swap on an occupied
// slot as [{new, s}, {0, s}], and the client drops the whole message when a
// put lands on an occupied slot (ropa-al-desmontar.md section 3.1). A plain
// "final item per slot" summary loses those removals, so each slot also keeps
// whether the template had something there: the first change to a slot says
// so (it carries {0, slot} only if the slot was occupied).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace cdmp {

class EquipState {
public:
    struct Slot {
        uint32_t item = 0;         // on the slot now; 0 = empty
        bool had_at_load = false;  // the template had something on it
    };
    using Items = std::vector<std::pair<uint32_t, uint16_t>>;  // {item, slot}

    // Equipment slots seen in the game go up to ~23; one change touches a few.
    static constexpr uint16_t kMaxSlot = 63;
    static constexpr size_t kMaxItems = 2 * (kMaxSlot + 1);  // a snapshot: each slot put and removed

    static bool valid(const uint8_t* p, size_t n) { return p && n >= 9 && (n - 9) % 6 == 0; }
    // A change a real player can make: few items, real slots (security review H5).
    static bool plausible(const uint8_t* p, size_t n) {
        if (!valid(p, n) || (n - 9) / 6 > kMaxItems) return false;
        for (size_t i = 9; i + 6 <= n; i += 6) {
            uint16_t slot;
            std::memcpy(&slot, p + i + 4, 2);
            if (slot > kMaxSlot) return false;
        }
        return true;
    }

    // Applies one change (or a snapshot); false, and nothing changes, if malformed.
    bool apply(const uint8_t* p, size_t n) {
        if (!plausible(p, n)) return false;
        uint32_t serial;
        std::memcpy(&serial, p + 5, 4);
        Items items;
        for (size_t i = 9; i + 6 <= n; i += 6) {
            uint32_t item;
            uint16_t slot;
            std::memcpy(&item, p + i, 4);
            std::memcpy(&slot, p + i + 4, 2);
            items.emplace_back(item, slot);
        }
        apply_items(serial, items);
        return true;
    }
    bool apply(const std::vector<uint8_t>& b) { return apply(b.data(), b.size()); }

    void apply_items(uint32_t serial, const Items& items) {
        for (const auto& [item, slot] : items) {
            if (slots_.count(slot)) continue;
            bool removal = false;
            for (const auto& [i2, s2] : items) removal |= s2 == slot && i2 == 0;
            slots_[slot].had_at_load = removal;
        }
        // A swap comes as [{new, s}, {0, s}]: removals first, then what is put on.
        for (const auto& [item, slot] : items) {
            if (item == 0) slots_[slot].item = 0;
        }
        for (const auto& [item, slot] : items) {
            if (item != 0) slots_[slot].item = item;
        }
        if (serial > serial_) serial_ = serial;
    }

    // Slots the template had filled: a copy loaded from it needs them emptied.
    std::vector<uint16_t> removals() const {
        std::vector<uint16_t> out;
        for (const auto& [slot, s] : slots_) {
            if (s.had_at_load) out.push_back(slot);
        }
        return out;
    }
    // What is on each changed slot now.
    Items puts() const {
        Items out;
        for (const auto& [slot, s] : slots_) {
            if (s.item) out.emplace_back(s.item, slot);
        }
        return out;
    }

    // Everything as one payload, like the server would send it: the puts,
    // then a removal for each slot the template had. Applying it to an empty
    // state gives this state back.
    std::vector<uint8_t> snapshot(uint32_t key) const {
        Items items = puts();
        for (uint16_t slot : removals()) items.emplace_back(0u, slot);
        return payload(key, serial_, items);
    }

    static std::vector<uint8_t> payload(uint32_t key, uint32_t serial, const Items& items) {
        std::vector<uint8_t> b(9 + 6 * items.size());
        b[0] = 4;
        std::memcpy(b.data() + 1, &key, 4);
        std::memcpy(b.data() + 5, &serial, 4);
        for (size_t i = 0; i < items.size(); ++i) {
            std::memcpy(b.data() + 9 + i * 6, &items[i].first, 4);
            std::memcpy(b.data() + 13 + i * 6, &items[i].second, 2);
        }
        return b;
    }

    bool empty() const { return slots_.empty(); }
    uint32_t serial() const { return serial_; }
    const std::map<uint16_t, Slot>& slots() const { return slots_; }
    void clear() {
        slots_.clear();
        serial_ = 0;
    }

private:
    std::map<uint16_t, Slot> slots_;
    uint32_t serial_ = 0;
};

}  // namespace cdmp
