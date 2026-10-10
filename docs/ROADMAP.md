# Roadmap

Everything is developed and tested on **a single PC**. The second player is played by
`cdmp_peer.exe` until milestone 5.

## Milestone 0: skeleton ✅ (2026-10-03)
- `cdmp.asi` loads into `CrimsonDesert.exe` 1.0.0.2976 via Ultimate ASI Loader (`winmm.dll`).
- Log with the exact game version (PE timestamp `0x6AB28F00`, base `0x140000000`).
- UDP networking with a handshake with retries, ping/RTT, timeout and reconnection. Tested with
  10% packet loss and 20 ms of jitter.
- The mod only activates inside `CrimsonDesert.exe`. The loader is also injected into
  `crashpad_handler.exe`, and there the mod does nothing.
- It does not touch the game's memory yet.

## Milestone 1: external reader (no injection) ✅ (2026-10-03)
- `cdmp_probe.exe` (read-only): signatures, strings, watch, dump, RTTI, vtables, findptr.
- Build identified: **patch 2.03.02**. 42/48 community signatures with a unique match.
- Verified live with a loaded save: position (global and physics), rotation,
  the actor's component table, HP and camera with projection.
- Key finding: internal client/server architecture with 1,116 `Troc*` messages.
- Details in `docs/research/hallazgos-en-vivo.md`. Missing: stamina and spirit ids.

## Milestone 2: real position inside the mod ✅ (2026-10-03)
- `PlayerReader` reads position, rotation, HP and stamina at 20 Hz, read-only and without
  hooks. Every pointer hop is validated by vtable and every read uses `VirtualQuery` + SEH.
- Tested in the game: the position matches the game's global to within
  0.1 m, the rotation follows turns and HP regenerates live. The ghost received
  more than 3,000 states with no rejections.
- Filters out the loading position (0, 1000, 0) and disables itself if the build is not 1.0.0.2976.
- Fast travel: a jump of about 790 m without losing the pointer chain, and with the check against
  the global within 0.05 m. Full session: 4,677 states received by the ghost, 0 rejections.

## Milestone 3: overlay ✅ (2026-10-03)
- ImGui on DX12 with **inline** hooks (safetyhook) on Present, Present1, ResizeBuffers
  and ResizeBuffers1, plus ExecuteCommandLists to get the present thread's queue.
- Tested with DLSS 4.5 and frame generation: draws at a real 76 FPS with no errors.
- Panel (F8 shows or hides it) with network, own HP and stamina, and the other player's HP and
  distance.
- Marker over the other player with name, HP bar and distance. The control crosshair
  lands on the player's own character (verified with a screenshot).
- Lessons from the failed attempt:
  - Patching the vtable does not work because Streamline calls Present directly.
  - Creating the test D3D12 device at startup left the game on a black screen. Now it is
    installed once `WorldSystem` exists, the window is visible, and after 5 more seconds.
- Kill switch: `[overlay] enabled=0` in `bin64\cdmp.ini`.
- `cdmp_dx12_test.exe` tests the hook without the game.

## Milestone 4: use the internal client/server messages (rethought on 2026-10-03)
The game keeps the architecture of the original MMO: 1,115 Troc messages as bytes over
an in-memory loopback, with one server session per user. See
`docs/research/troc-sintesis.md`.
- **4a:** passive sniffer on `ClientNet::PostPacket` and `ClientNet::OnRecvPacket`, logging
  only, to get a catalog of real traffic.
- **4b:** decode the key payloads (`MoveActor`, `UpdateVisiblePlayer`,
  `RemoveActor`).
- **4a ✅** sniffer v2 with full packets, 0 losses, and a capture kit for contributors.
- **4b ✅** formats decoded: `MoveActorReq`, `ActorLoadDataAck` (user and body,
  position 154 bytes from the end) and `RemoveActorAck`.
- **4c ✅** clone of Kliff by injecting two `ActorLoadDataAck` with new keys (F9).
  It appears next to the player, with collision. F10 removes it with a fade.
- **4d ✅** mirror mode: the player's `MoveActor`/`StartAction`/`RotateActor` Reqs are
  reinjected as Acks for the clone (formats per `mirror-ack-formats.md`), with a configurable
  delay.
  - Adjustments that made it work: readjusting timestamps, offsetting the
    `StartAction` positions and clearing the mount key.
  - Result in the test: works much better, with very occasional teleports.
  - Expected: the clone is a puppet (like a remote player in an MMO), it does not interact
    with the world. With a sideways offset the climbing animations do not line up
    with the wall; with `mirror_offset_m=0` it follows the exact path.
  - Climbing: ledge climbs with `UpdateWallUpDataAck` (it no longer disappears) and free climbing
    with a hook on `PointClimb::Enter` (it no longer gets stuck hanging). A slight sliding remains.
    Both will be needed anyway with a real player.

- **4e ✅** networked co-op (2026-10-03). Tested on one PC with `cdmp_peer --mode relay`,
  which sends your own data back to you: it looks good.
  - `local_capture` (sender): the player's Reqs, synthesized movements, the ledge path and
    loading templates.
  - `remote_puppet` (receiver): the other player as a remote actor, with all the mirror
    fixes. The F9/F10 mirror uses the same code.
  - Protocol v3: `GameEvent`, fragmented reliable channel (`RelData`/`RelAck`) and `SpawnInfo`
    (~31 KB).
  - The receiver estimates the game-clock difference between the two (minimum over 4 s, smoothed).
  - `cdmp_peer --mode relay`: forwards between two players or echoes with one. Reports the other
    player's name.
  - Package for a friend: `scripts\package_coop.ps1` → `dist\cdmp-coop-<date>.zip`.
  - Live tests on 2026-10-03 (protocol v4):
    - ✅ **equipment**: works since the copy has its own character number. The
      shared number mixed up the equipment screens, because the client's equipment cache
      is a single one and is keyed by character number.
    - ✅ the weapon was already switching by itself, with the actions: the docking relay is unnecessary.
    - ❌ **horse**: with an owner, the client does not create the copy; without an owner, the game closes.
      Forcing `RideOff::Enter` left the copy stuck. It stays disabled (`horse=0`).
  - Details of what was added:
    - equipment: hook on the server writer of `OtherEquipItemAck` and removing equipment from
      `UnequipItemAck`; on spawn, the changes since loading are applied
      (`apariencia-estado-coop.md`);
    - weapon in hand/on back: hook on the writer of `ChangeDockingSocketByFrameEventAck`
      (`combate-coop.md` §3);
    - horse, phase 1: a copy of the sender's horse is loaded, it moves with the rider's `Path.world`
      and from memory when nobody is riding it, and hooks on `RideOn`/`RideOff::Enter`
      (`caballo-coop.md`). Switch: `[coop] horse=0`;
    - orientation on spawn, and the default load at (0, 1000, 0) is ignored;
    - the overlay marker sits over the drawn character.
  - Missing: shared combat (enemies, damage; plan in `combate-coop.md` §5).

## Milestone 5: Steam (in progress)
- ✅ Steam transport over `ISteamNetworkingMessages` from the game's `steam_api64.dll`, without
  callbacks (`docs/research/steam-transporte.md`). Tested live up to "relay ready".
- ✅ Host and guest without `cdmp_peer` (`[net] role=host|guest`), also over UDP.
- Missing: first test with a real person.

## Later
Synchronize enemies and combat (with stable IDs across PCs, not pointers), animations,
quests and fast travel.
