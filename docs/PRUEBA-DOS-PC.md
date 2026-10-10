# Two-PC test (host + guest)

First serious session with a second player. It comes from the protocol in `docs/research/pendiente-robustez.md` §7,
adapted to what already works (phases 0-3 of `docs/PLAN-PENDIENTES.md`).

- **A** = host, with `cdmp.ini.anfitrion` (host profile) renamed to `cdmp.ini`.
- **I** = guest (the second player), with the `cdmp.ini` from the zip (the guest one).
- Duration: about 60 minutes, talking over voice chat.
- **Before each step, A presses Ctrl+F12** (a mark in the log) and says the number out loud
  ("mark 7: I travels"). If anything odd happens, both press Ctrl+F12 and write it down.
- **Nobody saves the game** during the whole test.

## 6 Oct session with a second player (in order of priority)

Section A already worked with Shadow (5 and 6 Oct). In this session:

1. **Tree with the rope** (the most important; fixes `cd34fd0` and `89508c8`). Both next to the
   same tree (it has to be loaded on both PCs: stay close).
   - **T1** A: grapple, pull until the tree bends and let go to fly off. On I: does A's copy
     grapple, bend the tree and fly off? Does the blue marker disappear on letting go?
   - **T2** A: repeat it 3 times on the same tree and 2 on another. On I: does it work every time or only the
     first?
   - **T3** I does the same and A watches.
2. **Combat** (section C, not yet tested with two PCs; with today's fixes to hits on the
   copies): C1, C2 and C3 above all.
3. **Life cycle** (section B, not tested either): B1, B3 and B4.
4. If there is time left, **A switches weapon** from the quick slot and keeps hitting: on I, does A's
   copy switch weapon too?

Ctrl+F12 at each step and at each odd thing, as always. Nobody saves.

## 0. Preparation (both)

1. Back up `%LOCALAPPDATA%\Pearl Abyss\CD\save` to another folder.
2. **The same zip on both.** Copy to `bin64`: `winmm.dll`, `cdmp.asi` and `cdmp.ini` (A: the
   host one, renamed).
3. Each one sets their `player_name`.
4. If on entering you get the red warning "Different mod versions", stop: one of you
   does not have the right zip.

## A. Connection and the basics (10 min)

| Step | Who | What | What to look at |
|---|---|---|---|
| A1 | A and then I | Open the game and load the save | Panel (F8): "connected to …" |
| A2 | Both | Fast travel to the same point | The other player's copy appears within a few seconds |
| A3 | Both | Walk, run, jump, climb, glide for 2 min | That the copy does not teleport or sink |
| A4 | Both | **Swim** for 1 min (never tested) | Does the copy swim, float or disappear? |
| A5 | I | Change a piece of equipment and draw the weapon | I's copy on A reflects it |
| A6 | Both | Call the horse, mount, ride together and dismount | That the copy mounts and dismounts correctly |

## B. Life cycle (15 min)

| Step | Who | What | What to look at |
|---|---|---|---|
| B1 | A | Fast travel far away and come back | On I, A's copy disappears and comes back, without looping flickers |
| B2 | I | The same | Same, on A |
| B3 | I | Open the map or the inventory for 30 s and close it | On A, does I's copy jump or speed up when coming back? |
| B4 | I | Die (for example, a high fall) and respawn | On A, I's copy dies, respawns and moves |
| B5 | A | Die and respawn | Same, on I |
| B6 | I | Exit to the main menu, wait 20 s and load again | On A, the copy leaves and comes back |
| B7 | I | **Close the game** and enter again | They reconnect by themselves |

## C. Combat (20 min)

Look for a small group of enemies (2-4). Note: I also sees the enemies from their own save in
the same place (they are not removed yet). If you can, pick a group that I has already killed in their save.

| Step | Who | What | What to look at |
|---|---|---|---|
| C1 | A | **Ctrl+F7** and start fighting | I sees the copies of A's enemies move and attack |
| C2 | I | Hit a copy until it dies | On A, the real enemy staggers, loses health and dies, with loot for A |
| C3 | I | Let the enemies hit them | I loses health and staggers with each hit; do they attack often or almost never? |
| C4 | I | Let A's enemies kill them | I dies with their normal animation and respawns |
| C5 | A | Fast travel in the middle of combat | On I, the copies disappear |
| C6 | I | Fast travel in the middle of combat and come back | That no frozen copies are left |

## D. Load (10 min)

| Step | Who | What | What to look at |
|---|---|---|---|
| D1 | Both | Large camp (8 or more enemies), 2 min of fighting with Ctrl+F7 | Stutters, lag, copies that do not appear |
| D2 | Both | City with lots of people, 2 min | Stutters or lag of the other player's copy |

## E. Enemy zone (phase 5, 20 min)

Within 150 m of A, I stops seeing **their own** hostile enemies and sees only the copies of
A's; beyond 170 m their own come back. Design: `docs/research/fase5-suprimir-enemigos-invitado.md`.
Before this, the one-PC test in `docs/PRUEBAS-COOP.md` (zone in dry run) must have gone well.

- **Preparation.** Back up the saves **on both**. `[sniffer] record=1` on both.
  - I: in their `cdmp.ini`, `[coop] suppress=1` (the kit ships `dry`, which only writes to the log).
  - A: nothing (it needs `enemy_auto=1` and `[dev] enemy_relay=1`, which the kit already ships).
- **Place:** the Bleed Bandits camp near (−10.284, 640, −3.383)
  (`cd_m11_m04_monster_beacon_01_0003`) or `Node_Her_EmbernestBeacon` (−10.201, 623, −3.481).
- In I's panel (F8) a line shows: "Host's enemies (150 m zone)" inside the zone and
  "Your enemies" outside.

| Step | Who | What | What to look at |
|---|---|---|---|
| E0 | Both | With `suppress=dry` on I: go to the camp together | Nothing changes. I's log: `Suppress: zone ON`, `WOULD DROP`, `WOULD SWEEP`; A's log: `the guest's enemy zone ON` and `WOULD copy N`. The two sets must match. Then I sets `suppress=1` and restarts the game |
| G1 | Both | Arrive at the camp together | I sees only A's copies, no duplicated bandits. I's log: `zone ON`, `dropped load`, `removed group`; A's log: `copying every zone enemy` and `sending enemy … (zone)` for the whole camp |
| G2 | I | Move 200 m away from A and come back | Beyond 170 m (5 s): the copies leave and I's bandits come back (`zone OFF (host far away), restoring N`, `restored group`, after 2 s `login +CF 1`). On coming back within 150 m they are hidden again |
| G3 | I | Fight their own bandits far from A; A approaches | The zone waits (up to 60 s) for the fight to end: `kept 0x… fighting you`. Afterwards they are hidden and the copies remain |
| G4 | A | Close the game | I turns off the zone (`link lost`) and gets their enemies back |
| G5 | I | Save with the zone on, exit and load that save **without the mod** | The camp is normal (only if G1-G4 went well; with a backup of the saves) |

If something goes wrong (enemies that do not come back, the game complains): I sets `suppress=0`, or moves about
250 m away and comes back (the server reloads their enemies), or goes through a loading screen. If
`Suppress: BREAKER` shows up in the log, the zone turns itself off until the game is restarted.

## What each one sends at the end (one zip per player)

- From `bin64`: `cdmp.log` and, if the game was restarted, `cdmp.1.log`, `cdmp.2.log`…;
- the session's `cdmp_packets_<date>.bin` (the packets `.log` is not needed);
- their `cdmp.ini`;
- the list of marks with what they saw at each one ("M7: A's copy stood still for 5 s");
- video or screenshots, if any;
- if the game closed by itself: the exact time.

## The session is good if

- There are no game crashes or hangs.
- A and B pass without having to restart the game.
- In combat: at least one of A's real enemies killed by I, and I wounded and staggered by
  A's enemies.
