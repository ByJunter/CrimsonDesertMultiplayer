#pragma once
// Time of day between the two games (hora-del-dia.md). Each game keeps its
// own clock of day and night from its own save; the host sends its current
// field time every few seconds and the guest has its own server set the same
// time with SetFieldTimeReq, as sleeping would.

#include <cstdint>
#include <optional>

#include "common/protocol.h"

namespace cdmp::net {

enum class TimeSyncMode { Off = 0, Send = 1, Apply = 2 };
void time_sync_set_mode(TimeSyncMode mode);
// Dev: Ctrl+Shift+F12 moves our own time of day 3 hours forward through the
// same SetFieldTimeReq the guest uses (a one-PC check of the mechanism).
void time_sync_set_dev_key(bool on);

// Sender: an event to send now, if it is time to.
std::optional<proto::GameEvent> time_sync_tick(uint32_t now_ms);
// Receiver: the other player's time of day.
void time_sync_on_event(const proto::GameEvent& ev);

}  // namespace cdmp::net
