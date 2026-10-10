#pragma once
// Reliable, ordered delivery of (large) messages over the unreliable datagram
// link. Pure logic, like ClientSession: the caller provides the clock and a
// send callback, so it is unit-testable without sockets.
//
// A message is split into RelData fragments of up to proto::kRelChunk bytes.
// The receiver acknowledges every fragment it keeps (duplicates too) and
// delivers messages exactly once and in the order they were sent (an enemy's
// spawn before its death before its removal). The sender keeps at most
// kWindow fragments in flight and resends unacknowledged ones until they are
// acknowledged or the channel is reset (reconnection).
//
// Each channel numbers its messages from a random first id, its "stream",
// which travels in every fragment: a new stream (the other side restarted or
// reset) restarts the receiver's ordering.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "common/protocol.h"

namespace cdmp {

class ReliableChannel {
public:
    static constexpr uint32_t kResendMs = 250;
    static constexpr size_t kMaxResendPerTick = 64;
    static constexpr size_t kWindow = 128;        // fragments in flight (unacknowledged)
    static constexpr size_t kMaxQueued = 2048;    // fragments waiting for the window (~2.2 MB)
    static constexpr size_t kMaxPartials = 64;    // incomplete messages held by the receiver
    static constexpr uint32_t kMaxAhead = 4096;   // messages a fragment may be ahead of the next one due
    // What the receiver holds for messages it cannot deliver yet (partial, or
    // complete but waiting for an earlier one). A peer that never sends the
    // next message due could otherwise fill memory with complete later ones
    // (security review: ~1 GB). The next message due is always let in.
    static constexpr size_t kMaxHeldBytes = 8 * 1024 * 1024;
    static constexpr size_t kMaxHeldMessages = 256;

    using SendFn = std::function<void(proto::Body)>;
    struct Message {
        proto::RelKind kind;
        std::vector<uint8_t> bytes;
    };

    explicit ReliableChannel(SendFn send);

    // Fragments and queues; sends what fits in the window now. False if the
    // message is too large or the queue is full.
    bool send(proto::RelKind kind, const std::vector<uint8_t>& bytes, uint32_t now_ms);

    // Resends what is still unacknowledged and sends what the window allows.
    void tick(uint32_t now_ms);

    void on_ack(const proto::RelAck& ack);

    // Acknowledges the fragment when it is kept, and returns the messages it
    // made deliverable, in order (often none, sometimes several).
    std::vector<Message> on_data(const proto::RelData& data);

    // Forget everything (new connection); outgoing messages start a new stream.
    void reset();

    size_t unacked_fragments() const { return outgoing_.size() + queued_.size(); }
    uint64_t resent() const { return resent_; }
    uint64_t refused() const { return refused_; }  // fragments not kept (no room, too far ahead, old stream)

private:
    struct Outgoing {
        proto::RelData data;
        uint32_t last_sent_ms;
    };
    struct Partial {
        proto::RelKind kind;
        uint16_t count;
        uint16_t have = 0;
        std::vector<std::vector<uint8_t>> chunks;
        std::vector<bool> got;
    };
    void flush(uint32_t now_ms);

    SendFn send_;
    // Sending side.
    uint32_t stream_ = 0;
    uint32_t next_id_ = 0;
    std::deque<proto::RelData> queued_;      // waiting for room in the window
    std::map<uint64_t, Outgoing> outgoing_;  // in flight; key: msg_id << 16 | index
    // Receiving side.
    uint32_t rx_stream_ = 0;
    uint32_t rx_next_ = 0;                  // the next message id to deliver
    std::set<uint32_t> old_streams_;        // streams we moved away from: their stragglers are ignored
    std::deque<uint32_t> old_order_;        // the same, oldest first (the oldest is forgotten first)
    size_t held_bytes_ = 0;                 // chunk bytes in incoming_ and ready_
    void forget_stream(uint32_t stream);
    std::map<uint32_t, Partial> incoming_;  // msg id -> fragments so far
    std::map<uint32_t, Message> ready_;     // complete, waiting for earlier ones
    uint64_t resent_ = 0, refused_ = 0;
};

}  // namespace cdmp
