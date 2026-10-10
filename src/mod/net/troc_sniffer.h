#pragma once
// Passive logger for the game's internal client<->server messages ("Troc").
// Hooks the two byte-level funnels with mid-hooks at their entry points and
// only COPIES packets out; nothing is modified, blocked or delayed beyond a
// short memcpy. A writer thread turns the copies into:
//   cdmp_packets.log  one line per packet (direction, name, opcode, hex preview)
//                     plus per-opcode statistics every 10 s
//   cdmp_packets.bin  full packet bytes for offline analysis (format in .cpp)

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cdmp::net {

// `dir` is the mod's folder; `stop` is the mod-wide stop event. With
// record=false the hooks still feed co-op but nothing is written to disk.
bool sniffer_install(const std::wstring& dir, HANDLE stop, bool record = true);

// Experimental (combat tests): queue a client->server packet (u16 opcode |
// u8 slot | u16 len | payload) for the game's own in-process server. It is
// posted from inside the next real ClientNet::PostPacket call, on that game
// thread, with the channel/flags real packets use. False if not possible.
bool sniffer_inject_c2s(std::vector<uint8_t> packet);
// The world is loading (GameLoadingStartReq until 1.5 s after
// LoadingCompleteAck): nothing of ours goes in either way. Any thread.
bool sniffer_world_loading();

}  // namespace cdmp::net
