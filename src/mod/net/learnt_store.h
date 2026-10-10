#pragma once
// cdmp_learnt.txt next to cdmp.asi: the combat values guest_hits and
// player_hits learn in this player's game, kept between sessions
// (common/learnt_file.h has the format and its bounds).

#include <string>

#include <windows.h>

namespace cdmp::net {

// Before the sniffer's hooks learn anything new: puts the file's values in.
void learnt_load(const std::wstring& dir);
// Writes the file every 30 s while something new was learnt, until `stop` is
// set (and once more then). Own thread.
void learnt_save_loop(const std::wstring& dir, HANDLE stop);

}  // namespace cdmp::net
