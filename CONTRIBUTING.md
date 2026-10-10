# Contributing

This project is **no longer maintained** by its author: issues and pull requests may never be
answered. The best way to continue it is to **fork it** and build your own version (keeping the
GPL v3). These notes are what you need to know to work on it safely.

## Before you start

- **Game build.** Everything (offsets, signatures, message ids, the tables in `src/common/*_2976.*`)
  is for Crimson Desert **1.0.0.2976** (game version 2.03.02, PE timestamp `0x6AB28F00`, fixed
  image base `0x140000000`). Write the build next to any new finding. A game update means
  finding every address again.
- **Back up your saves** (`%LOCALAPPDATA%\Pearl Abyss\CD\save`) before any test that **writes**
  to the game's memory or injects messages.
- **One game per PC.** Steam and the game's protection do not allow two copies on the same PC.
  For a first test, use `cdmp_peer.exe` on one PC (your own character comes back as the "other
  player"); for real two-player tests you need two PCs and two Steam accounts that own the game
  (see `docs/PRUEBA-DOS-PC.md`).
- The main documents in `docs/` are in English; the research notes in `docs/research/` are in
  Spanish (`docs/research/README.md` is an English index).

## Commands

- Build and run the unit tests: `.\scripts\build.ps1` (finds the VS Build Tools by itself; CMake
  does not need to be on the PATH).
- Install into the game: `.\scripts\deploy.ps1`; uninstall with `-Remove`. It fails if the game
  is running.
- Two-PC test package: `.\scripts\package_coop.ps1`. Public package:
  `.\scripts\package_release.ps1` (profiles and texts in `kit\public\`, version in
  `CMakeLists.txt`).
- Full test without the game: start `cdmp_peer.exe`, then
  `cdmp_host.exe <path\cdmp.asi> <seconds>`. `LoadLibrary` fails with long paths (error 206),
  so use a short path such as `build\e2e\`.
- Test in the game: start `cdmp_peer`, launch the game (`steam://rungameid/3321460`) and read
  `bin64\cdmp.log`. The game ignores `WM_CLOSE`: on the title screen, close it with
  `Stop-Process`.

## Engineering rules

A crash of the game is reported to Pearl Abyss by its crash handler, so these rules matter.

- **No inline hooks in the middle of a function.** For injection points in the middle of a
  function use `safetyhook::create_mid`.
- **Every read of game memory is guarded** (`VirtualQuery` and/or SEH).
- **Never identify entities across PCs by their memory address.** Use the game's actor keys
  translated by the mod, or the mod's own ids.
- **No work inside `DllMain`.** Everything runs on the worker thread.
- The loader (`winmm.dll`) is also loaded into `crashpad_handler.exe`; `dllmain.cpp` filters by
  executable name. Keep it that way.
- **Overlay:** only inline hooks (SafetyHook) on functions, never vtable patches (Streamline
  calls Present directly). Install it **deferred** (after `WorldSystem` exists and the window is
  visible): installing it at startup hung the game. Test overlay changes with
  `cdmp_dx12_test.exe` before the game.
- **The protocol is serialized field by field** (`src/common/protocol.cpp`); if the format
  changes, bump `kVersion` so mismatched versions refuse to connect.
- **Safety of the people who use it:** keep accepting Steam friends only, validate every field
  that comes from the network (sizes, counts, keys), and never let the other player's data
  reach game functions unchecked. `docs/research/seguridad/` has the reviews that were done.

## Third-party code

- `reference/` (ignored by git) was used for a clone of another co-op attempt that **has no
  license**: it may be read as a reference but none of its code may be copied. Its analysis is in
  `docs/ANALISIS-CrimsonDesertCoop.md`.
- New dependencies must have a license compatible with the GPL v3 and be added to
  `kit/public/THIRD-PARTY-NOTICES.txt`.
