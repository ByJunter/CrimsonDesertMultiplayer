#pragma once
// Ring buffer of variable-length records over caller-provided storage.
//
// Every record starts with a u32 record length (header + data). A length of 0
// is a "wrap" marker: the rest of the ring up to its end is unused and the
// next record starts at offset 0. If fewer than 4 bytes remain before the end,
// the consumer wraps without a marker. head/tail are monotonic byte counters.
//
// Not thread-safe by itself. Intended use (troc_sniffer): producers call
// reserve()+commit() under a lock; the single consumer snapshots head() under
// the same lock, drains [tail, head) WITHOUT the lock (producers never write
// there because free space is computed from tail), then publishes the new
// tail under the lock with set_tail().

#include <cstdint>
#include <cstring>
#include <optional>

namespace cdmp {

class ByteRing {
public:
    ByteRing(uint8_t* storage, uint64_t capacity) : data_(storage), cap_(capacity) {}

    // Returns the offset where a record of `len` bytes (len >= 4) must be
    // written, or nullopt if there is no room. Must be followed by commit(len)
    // once the record (starting with its u32 length) is in place.
    std::optional<uint64_t> reserve(uint32_t len) {
        const uint64_t pos = head_ % cap_;
        const uint64_t to_end = cap_ - pos;
        const uint64_t need = len <= to_end ? len : to_end + len;
        if (len > cap_ || cap_ - (head_ - tail_) < need) return std::nullopt;
        if (len <= to_end) return pos;
        if (to_end >= sizeof(uint32_t)) std::memset(data_ + pos, 0, sizeof(uint32_t));
        head_ += to_end;  // skip to the start of the ring
        return 0;
    }
    void commit(uint32_t len) { head_ += len; }

    uint8_t* at(uint64_t offset) { return data_ + offset; }
    uint64_t head() const { return head_; }
    uint64_t tail() const { return tail_; }
    void set_tail(uint64_t tail) { tail_ = tail; }

    // Calls visit(const uint8_t* record, uint32_t len) for each record in
    // [from, to) and returns the position after the last one.
    template <class Visit>
    uint64_t drain(uint64_t from, uint64_t to, Visit&& visit) const {
        while (from < to) {
            const uint64_t pos = from % cap_;
            const uint64_t to_end = cap_ - pos;
            if (to_end < sizeof(uint32_t)) {
                from += to_end;
                continue;
            }
            uint32_t len = 0;
            std::memcpy(&len, data_ + pos, sizeof len);
            if (len == 0) {
                from += to_end;
                continue;
            }
            visit(static_cast<const uint8_t*>(data_ + pos), len);
            from += len;
        }
        return from;
    }

private:
    uint8_t* data_;
    uint64_t cap_;
    uint64_t head_ = 0;
    uint64_t tail_ = 0;
};

}  // namespace cdmp
