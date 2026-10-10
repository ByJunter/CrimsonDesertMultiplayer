# Crimson Desert Multiplayer (cdmp)

Unofficial two-player co-op mod for **Crimson Desert** (PC, Steam), written from scratch in C++20.
It loads into the game as `cdmp.asi` through Ultimate ASI Loader.

> **Not maintained. Alpha, or even less.**
> This is a proof of what co-op in Crimson Desert could become, not a finished multiplayer
> mode. It has known bugs and unfinished parts, and I am not developing it any further.
> The code is here, under the GPL v3, so that anyone can continue it or use it as the base of
> their own project. Pull it apart, fix it, build on it — that is the whole point.

- **Download the ready-to-use mod:** [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/3677)
  (or the release on this page). Installation and known limitations are in
  [`kit/public/README.txt`](kit/public/README.txt) (Spanish: [`LEEME.txt`](kit/public/LEEME.txt)).
- **Game build:** 1.0.0.2976 (game version 2.03.02). Every address and signature in this
  repository is valid only for that build. With any other version the mod turns itself off.

## What it does

- Each player plays in **their own save** and sees the other as a character in their world:
  movement, climbing, gliding, combat actions, horse, gear, the magic rope on trees.
- Shared time of day (the guest takes the host's), the other player on the minimap and map.
- **Shared enemies (experimental):** near the host, the guest sees copies of the host's
  enemies; the guest's blows hurt, stagger and kill the host's real enemies, and those enemies
  attack the guest back.
- Connection through Steam networking, **Steam friends only**.

## How it works (short version)

Crimson Desert keeps the client/server architecture of an MMO inside a single-player game:
the client talks to a local "server" through internal messages ("Troc").

- `src/mod/net/troc_sniffer`: mid-hooks on the two game functions that carry every
  client↔server message; it can read them and inject new ones.
- `src/mod/net/local_capture`: your movement and action messages, plus data read from memory
  (gear, weapon, horse, rope).
- `src/mod/net/coop` and `src/common/*`: protocol, a reliable fragmented channel, sessions and
  transports (UDP for local tests, Steam for real games).
- `src/mod/net/remote_puppet`: loads a copy of the other player the way the MMO did
  (`ActorLoadDataAck`) and replays their messages as the server would have relayed them.
- `src/mod/net/enemy_relay`, `native_suppress`, `guest_hits`: the shared-enemy zone (host
  authority: the host's enemies are copied to the guest, the guest's hits go back to the host).

The research behind every piece (offsets, message formats, captures, failed attempts) is in
[`docs/research/`](docs/research/). Start with [`docs/ROADMAP.md`](docs/ROADMAP.md), then the
[research index](docs/research/README.md), which summarizes every note in English. The main
documents in `docs/` are in English; the research notes themselves are in **Spanish**.

## Layout

| Folder | Contents |
|---|---|
| `src/common/` | Protocol, reliable channel, session, transports, clock (shared, unit-tested) |
| `src/mod/` | The mod: startup, config, overlay, memory reading, network and co-op |
| `tools/peer_sim/` | `cdmp_peer.exe`: relay/echo to test on one PC |
| `tools/host/` | `cdmp_host.exe`: loads `cdmp.asi` without the game |
| `tools/probe/` | `cdmp_probe.exe`: external, read-only memory reader |
| `tools/dx12_test/` | Test window for the overlay hooks |
| `tools/scripts/` | Python helpers to analyse captures and the executable |
| `tests/` | Unit tests |
| `scripts/` | Build, install and packaging (PowerShell) |
| `kit/` | Config files and readmes of the packages (`kit/public/` = the public release) |
| `docs/` | Roadmap and test plans (English), research notes (Spanish, with an English index) |
| `third_party/` | Ultimate ASI Loader v9.7.4 (x64, MIT) |

## Building

Requirements: Windows 10/11 x64 and **Visual Studio 2022 or its Build Tools** with the
"Desktop development with C++" workload (MSVC x64 and the bundled CMake). Dear ImGui and
SafetyHook (with Zydis) are downloaded by CMake on the first configure.

```powershell
.\scripts\build.ps1               # configure, build (RelWithDebInfo) and run the unit tests
.\scripts\deploy.ps1              # copy the mod into the game's bin64 (fails if the game is running)
.\scripts\deploy.ps1 -Remove      # uninstall
.\scripts\package_release.ps1     # public zip (profiles and texts in kit\public\)
```

`deploy.ps1` installs into `D:\SteamLibrary\steamapps\common\Crimson Desert\bin64` by default;
use `-GameDir "<your game folder>\bin64"` (or the `CDMP_GAME_DIR` environment variable) for another path.

Testing on one PC (your own character comes back through the network as "the other player"):

```powershell
.\build\bin\RelWithDebInfo\cdmp_peer.exe --mode relay --delay 50 --offset 0
```

Then set `[net] transport=udp` and `[coop] enabled=1` in `bin64\cdmp.ini`, start the game and load
your save. The mod writes its log to `bin64\cdmp.log`.

Before contributing, read [CONTRIBUTING.md](CONTRIBUTING.md): it has the engineering rules
that keep the mod from crashing the game.

## Safety and privacy

- The mod only talks to the other player, through Steam networking, and only accepts Steam
  friends. There is no telemetry and no server of its own.
- Packet recording (`[sniffer] record`) is **off** by default. When it is on, the captures and
  `cdmp.log` contain in-game data (positions, actor keys); review them before sharing.
- Always back up your saves (`%LOCALAPPDATA%\Pearl Abyss\CD\save`) before testing, and do not
  save while the other player is in your world.

## Made with AI

This mod was developed by ByJunter with the help of **Claude**, an AI by Anthropic. The
documents in `docs/research/` are the working notes of that process: findings, dead ends and
fixes, dated, in the order they happened.

## License

Copyright (C) 2026 ByJunter.

This program is free software: you can redistribute it and/or modify it under the terms of the
**GNU General Public License version 3** ([LICENSE](LICENSE)). It comes with **no warranty**.
A modified version you publish must come with its source code under the same license.

Third-party components keep their own licenses
([kit/public/THIRD-PARTY-NOTICES.txt](kit/public/THIRD-PARTY-NOTICES.txt)):
Ultimate ASI Loader (MIT), Dear ImGui (MIT), SafetyHook (BSL-1.0), Zydis and Zycore (MIT).

*Crimson Desert and Pearl Abyss are trademarks of Pearl Abyss Corp. This project is not
affiliated with or endorsed by Pearl Abyss and contains no game files or game content. Steam is
a trademark of Valve Corporation.*

---

**Español:** mod cooperativo no oficial para dos jugadores. Ya no está en desarrollo; el código
está aquí con licencia GPL v3 para quien quiera continuarlo. Los informes de
investigación (`docs/research/`) están en español. Instrucciones de instalación: [`kit/public/LEEME.txt`](kit/public/LEEME.txt).
