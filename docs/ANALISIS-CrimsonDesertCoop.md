# Analysis: blizz3010/CrimsonDesertCoop

Source: https://github.com/blizz3010/CrimsonDesertCoop (cloned into `reference/CrimsonDesertCoop`)
Analysis date: 2026-10-03 · Last commit of the repo: 2026-05-22 (`c0a8938`), version 0.2.3

## What it is

An ASI mod (a DLL loaded by Ultimate ASI Loader) in C++20 for Crimson Desert that tries to
add 2-player co-op:

- **Transport:** Steam P2P (`ISteamNetworkingSockets`) + Steam friends lobby for invites.
- **Model:** the host is the authority. Player 2 appears in the host's world by
  "hijacking" an NPC companion (Oongka/Yann/Naira): it removes the companion's AI and writes
  its position into memory.
- **Hooks:** safetyhook (inline + mid-hooks) on AOB signatures taken from cheat tables
  and from other community mods.
- **UI:** ImGui overlay with a hook on DX12 `Present`. F7 = host, F8 = show/hide overlay.
- **Dependencies** (downloaded by CMake): safetyhook 0.4.1, spdlog, nlohmann/json, imgui.
  The Steamworks SDK has to be added by hand.

Size: about 8,100 lines, 38 files. It has no tests (the `CDCOOP_BUILD_TESTS` option
points to a `tests/` folder that does not exist) and no CI.

## Origin and reliability

- Most of the commits were written by an AI agent ("Claude") and merged
  via PR. There is no evidence that it has ever been run inside the game: no screenshots,
  no logs, no user issues confirming that it works.
- The offsets that the README marks as **"Verified"** come from cheat tables and mods
  by third parties (bbfox0703, Orcax, EquipHide, CDAnimCancel…). They are "verified" by
  those authors for *their* uses, not tested inside this mod.
- **License:** the repo has no LICENSE file; it only says "for educational and
  personal use". Legally we must **not** copy its code into our project. It
  can be used as a reference, and its links to public research can be used.

## Critical problems found (by reading the code)

### 1. The inline hooks are placed in the middle of functions → crash very likely
`hooks.h:44-57` always creates the signature hooks with `safetyhook::create_inline`. But
the signatures `POSITION_*`, `DAMAGE_SLOT_PRIMARY`, `STAT_WRITE_*` and `CAMERA_ZOOM_FOV`
(`game_structures.h:656-680`) are Cheat Engine injection points in the **middle of
a function**, not function entries. In addition, the `*_OFFSET` constants exist but
are never used.

The detours are shaped like normal functions (`player_position_detour(void*, float, float, float)`,
`hooks.cpp:595`) and call `hook.call<>()` as if they were calling the original function.
At a mid-function point this breaks the stack and the registers. Since these hooks
are installed **by default**, the most likely outcome is that the game crashes the first time
it runs through that code (for example, when moving). The code's own comment
acknowledges that the site is "mid-function". Only the hooks added later
(dragon, teleport, mount) correctly use `create_mid_hook`.

### 2. The session never gets connected
`Session::join_session` (`session.cpp:~100`) sends the `HANDSHAKE` right after
`ConnectP2P`. But `Session::send` requires `t->is_connected()` to be true, and
`SteamNetworkTransport::send` requires `impl_->connected`. That value is only set to
`true` in the `Connected` callback, which arrives later, inside `RunCallbacks()`.
Result: the handshake is silently dropped and there is no retry.

- The host stays in `HOSTING` and the client in `CONNECTING`.
- `is_active()` only returns true in `CONNECTED` (`session.h:79`).
- Therefore **no sync system ever runs**, the companion
  is never hijacked and enemy scaling is not applied.

### 3. The sync is one-way
Only the **host** hijacks a companion, and it does so on receiving the handshake. The client
receives the host's position, but `CompanionHijack` is not active on its side, so
**the client never sees the host**. Each player stays in their own single-player world.

### 4. The "companion hijack" probably targets the wrong entity
The player actor's slots `+0xD0..+0x108` ("body slots") come from EquipHide,
a mod that hides **equipment**, and they are resolved as `ChildActor`. Most likely
they hold weapons, armor or accessories, not the companions. "Disabling the AI"
consists of setting a pointer at `+0x48` to 0 (`companion_hijack.cpp`), which
will probably break or crash the entity rather than cleanly disable its AI.

### 5. The enemy sync cannot work
- `EnemySync` walks `actor_manager_ptr + 0xD0..0x108` (`enemy_sync.cpp`). Those are the
  offsets of the **player actor's** slots, applied to the **ActorManager**: they do not
  enumerate enemies, and at most there would be 8.
- Each enemy's ID is `pointer & 0xFFFFFFFF`. Addresses differ on
  each PC, so the client will never find the same enemy. The damage
  and death messages never match anything.

### 6. Other
- `on_remote_full_state` does a `memcpy` of the whole packet (header included) into the
  buffer. It works because the buffer has the same packet type, but the header
  ends up mixed into the interpolated state.
- `read_mem`/`write_mem` only check the pointer's range, not that the memory is
  readable. If a pointer goes stale (after a loading screen) → access violation.
- Animation, quest, cutscene, world interaction and
  fast travel sync on the receiver are stubs or only write to the log (the README admits this).
- The `ANIM_EVALUATOR` signature uses an inline `create_hook` with a `(void* evaluator)` detour.
  If that AOB is not a function entry either, it has the same risk as point 1.
- Steam's overlay and the mod's `SetRichPresence` may clash with the
  game's own presence.

## What can be reused

1. **Map of public research:** the README's lists of related projects
   are the most valuable part: Orcax player-status-modifier, CDGuardCancel (`.paac` animation
   format), CrimsonDesertTools/EquipHide (WorldSystem → ActorManager chain),
   bbfox0703 CT, CD Companion (teleport via physics delta, static XYZ,
   camera heading), PAZ unpackers and pycrimson.
2. **Documented pointer chains** (for us to check with Cheat Engine/ReClass):
   - Player: `WorldSystem → +0x30 ActorManager → +0x28 UserActor`
   - Position: `actor → +0x40 → +0x08 → core → +0x248 → +0x90` (float XYZ), quaternion at `+0xA0`
   - Stats: component at `actor+0x58`, `StatEntry` int64 ×1000
3. **Architecture ideas that are conceptually right:** authoritative host, Steam P2P
   with a friends lobby, position over an unreliable channel at 30 Hz and events over a reliable channel,
   a startup marker file (`cdcoop_loaded.txt`) for diagnostics, and an
   input thread independent of the overlay.
4. `tools/offset_scanner.py` and `docs/REVERSE_ENGINEERING.md` as a workflow guide.

## Conclusion

It is **a well-documented scaffold, not a working mod**. As it stands, the most
likely outcome is that it crashes when installing the default hooks (point 1), and even if it does not
crash, it never completes a connection (point 2). What it really contributes is the
collection of community offsets and research.

### Recommendation for our project
- Do **not** fork it as a code base (license and design flaws). Start from scratch
  using this repo and its sources as a reference.
- First realistic milestone, in this order:
  1. A minimal ASI that loads and writes a log.
  2. Read the local player's position with the documented chain, verifying it in the game.
  3. A real P2P connection between two PCs with handshake and retries.
  4. Show the other player as an overlay marker (without touching game entities).

  Only after that, research how to spawn or possess an entity safely,
  using mid-hooks and verifying everything with a debugger.
