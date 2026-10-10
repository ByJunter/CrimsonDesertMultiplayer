# Co-op tests (version of 2026-10-03, protocol v4)

Everything on this list is written and compiled, but **not tested in the game**. It is tested on a
single PC with `cdmp_peer --mode relay`, which sends your own data back to you: the "other player" is
you, with about 0.4 s of delay.

## Preparation
1. Back up the saves (`%LOCALAPPDATA%\Pearl Abyss\CD\save` → `backups\`).
2. Start the bridge:
   `build\bin\RelWithDebInfo\cdmp_peer.exe --mode relay --delay 50 --offset 0`
3. `bin64\cdmp.ini` must have `[coop] enabled=1`. It already does.
4. Open the game, load the save and move around. Your copy appears.
5. When you are done, exit **without saving**.

## What to test and what you should see

| # | Test | What should happen | `cdmp.log` line |
|---|---|---|---|
| 1 | Walk, run, jump, ledges, climb | Same as before | `Co-op: the other player is in our world` |
| 2 | **Change a piece of equipment** | The copy changes it ~0.4 s later | `Capture: equipment change`, `Puppet: equipment change` |
| 3 | **Take off a piece** | The copy takes it off | same |
| 4 | **Draw and sheathe the weapon** | The copy moves it from the back to the hand and vice versa | `Capture: weapon docking event` |
| 5 | **Call the horse and mount** | A second horse appears (the copy) and the player's copy mounts it | `Capture: horse template`, `Puppet: horse queued`, `Puppet: mount/dismount enabled` |
| 6 | **Ride** | Both horses move together and the rider stays in the saddle | — |
| 7 | **Dismount** | The copy gets off the horse | `mount/dismount enabled` |
| 8 | Take damage | Nothing visible (the copy's health changes internally) | — |
| 9 | Get wet or get burned | The copy too (elemental effect) | — |
| 10 | Die | The copy dies, and when you revive it revives | — |

## The copy sinks or floats (normal with a single PC)
- With one PC, the copy appears offset by `[coop] offset_m` meters in X, but at **your** height. On
  sloped terrain, the ground on that side is higher or lower, and the copy (which does not snap to the
  ground: it goes at the height it receives) ends up buried or floating. Measured: 0.35 m on average with 4 m,
  0.5 m with 12 m, up to 1.7 m (`docs/research/pendiente-copia-se-hunde.md`).
- **With two PCs it does not happen:** the kit uses `offset_m=0` and each player sends their real position on the same
  terrain.
- For movement or visual tests, the copy can go **behind you** instead of to one side:
  `offset_m=0.01` (avoids the 2.5 m that are set automatically with `cdmp_peer`) and `delay_ms` from 1500 to 3000.
- Large separations (4-10 m) are only needed for the tests of hits on the copies.

## Enemy zone on one PC (phase 5, dry run: writes nothing)
With `cdmp_peer` this PC is host and guest at the same time, so you can only check **what**
the guest would hide and **what** the host would copy: the mod forces the dry run
(`docs/research/fase5-suprimir-enemigos-invitado.md` §6.2, tests L0 and L0b).
1. In `bin64\cdmp.ini`: `[coop] suppress=dry` (or without the key: it is the default),
   `enemy_auto=1` and `[dev] enemy_relay=1`. `role=guest` with `transport=udp`, as always.
2. `cdmp_peer.exe --mode relay --delay 50 --offset 0`, open the game and load the save.
3. When your copy appears, after 2 s: `Suppress: zone ON (host 0 m away, N natives to hide) - DRY RUN`
   (0-3 m: your copy) and `Enemy relay: the guest's enemy zone ON (request 1, 150 m, dry run): only
   logging what we would copy`. In the panel (F8): "Zone 150 m (test): your enemies stay".
4. Go to the Bleed Bandits camp near (−10.284, 640, −3.383).

| # | What to do | What you should see in `cdmp.log` |
|---|---|---|
| L0 | Approach the camp | `Suppress: WOULD SWEEP group 0x… (N natives, "Bleed Bandit", … m from the host)` once within 150 m, and some `WOULD DROP load 0x… "Bleed Bandit" faction 1000016 … origin S` (few: almost everything loads at about 200 m). **Only bandits**: no guards, animals, civilians, riders or mercenaries |
| L0 | Go to a city | No `WOULD DROP` or `WOULD SWEEP` |
| L0b | Stay next to the camp | `Enemy relay: zone (the guest's dry run) WOULD copy N of M zone enemies within 150 m: …` with the same enemies as the guest's `WOULD SWEEP` (which counts by groups: leader and members) |
| — | Every 10 s | `Suppress: zone on`, `held 0` and `dry run: would drop N, would sweep N` (held always 0: nothing changes in the game) |

**Lab (optional, writes into the game: back up the saves and do not save).** Without `cdmp_peer`:
`[coop] suppress=1`, `suppress_lab=1` and `suppress_lab_radius_m=60`. **Ctrl+Home** turns
a zone around you on or off (no host; automatic copies stay off).
- L1b: next to a camp at rest, Ctrl+Home → after 2 s they disappear (`removed group`).
- L2: wait 30 s and Ctrl+Home again → they come back in place (`restored group`, `login +CF 1`),
  attack and can be killed.
- L1: with `suppress_lab_radius_m=250`, turn it on more than 250 m from the camp and approach → the
  camp does not appear (`dropped load`).

## If something goes wrong
- **The horse causes problems:** `[coop] horse=0` in `cdmp.ini`.
- **Something odd in general:** `[coop] enabled=0` disables all of co-op.
- Send me the `cdmp.log` and, if you can, a video.

## Connecting with a friend
- **Package:** `dist\cdmp-coop-<date>.zip`, with instructions in `LEEME.txt`.
- **Over Steam:** no port forwarding (`transport=steam`; one player sets `role=host`).
  - Verified on your PC that Steam initializes and the relay network becomes ready.
  - The real connection between two accounts is untested.
- **Over IP:** with a VPN or with the port open (`transport=udp`).

## Combat tests (optional; they write into the game)
They are used to decide how to implement shared damage (`docs/research/combate-coop.md` §4 and §6).
**Back up the saves and do not save afterwards.**
1. In `cdmp.ini`, section `[dev]`, add `combat_tests=1`.
2. Stand near an enemy (within 15 m) that is moving.
3. `Ctrl+F6` (test T2): takes 5 health from it through the game's internal server. Press it
   several times. Does its health go down? Does it die? Does it drop loot?
4. Hit the enemy once and then press `Ctrl+F7` (test T3): a "collision" hit with the
   hash of your last attack. Does it react? Does it lose health?
5. Tell me what you saw. The `cdmp.log` and `cdmp_packets_*.log` keep the rest.
