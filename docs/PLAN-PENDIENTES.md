# Plan for what remains to be solved (4 October 2026)

It comes from the 10 reports `docs/research/pendiente-*.md` and from the work on part 4
(`pieza4-*.md`). Game build: 1.0.0.2976.

Each step is closed with a test in the game (one PC with `cdmp_peer`, unless stated
otherwise), a commit and, where relevant, an update of the report. The next one is not started without closing
the previous one, except for the steps marked as independent.

---

## List of what remains to be solved

| # | Problem | Report | Size |
|---|---|---|---|
| 1 | **Pending test:** the guest's hits on the copies (damage, death, loot, stagger) | `pieza4-tambaleo.md` | Being tested |
| 2 | Cheat kills (`KillCheatReq`, "fatal" state) do not give skill points or count toward challenges | `pendiente-recompensas-progreso.md` | Small |
| 3 | 18 key checks stop working after 30-60 min (keys `0xB02…`); `0xA0100001` hard-coded; `net_id` unbounded | `pendiente-rango-claves.md` | Small |
| 4 | Arrow damage miscalculated in both directions (type 1 accessor misread) | `pendiente-proyectiles.md` | Small |
| 5 | Automatic mode copies animals; riders and towers without a clear rejection | `pendiente-jinetes-animales.md` | Small |
| 6 | Enemies can keep attacking a dead copy; `PlayerHit` is sent with the copy dead | `pendiente-reaccion-muerte-invitado.md`, `pendiente-aggro-y-respuesta-golpes.md` | Small |
| 7 | The guest's hit is applied on the host ~300 ms before their sword swing is seen | `pendiente-aggro-y-respuesta-golpes.md` | Small |
| 8 | **No version check:** the friend's kit is old; Ctrl+F7 saturates the link; if the versions do not match, they search for each other forever | `pendiente-robustez.md` | Medium |
| 9 | On the guest's PC the enemy copies target the host's copy (`0xA010F001` not translated) | `pendiente-proyectiles.md`, `pendiente-aggro-y-respuesta-golpes.md` | Small (changes the protocol) |
| 10 | Reliable channel: a message can stay incomplete forever; enemy messages out of order; sizes not validated | `pendiente-robustez.md` | Medium |
| 11 | Enemy relay without a life cycle: reconnection, loading screens, "out of the world" flickers, old packets after a menu | `pendiente-robustez.md` | Medium |
| 12 | The log is wiped at startup; data missing to debug with the friend; one host `.ini` and another guest one | `pendiente-robustez.md` | Small |
| 13 | The guest does not react to hits from the host's enemies and never dies from them | `pendiente-reaccion-muerte-invitado.md` | Medium |
| 14 | Sharing enemies between the two players (today almost all go to the host) | `pendiente-aggro-y-respuesta-golpes.md` | Medium |
| 15 | Visual response to the guest's hit: impact effect on the host, damage numbers on the guest, less delay | `pendiente-aggro-y-respuesta-golpes.md` | Medium |
| 16 | The guest sees their own enemies in addition to the copies (duplicates) | `pendiente-3g-suprimir-enemigos-invitado.md` | Large |
| 17 | The guest does not receive loot, experience or quest progress | `pendiente-recompensas-progreso.md` | Medium (depends on 16) |
| 18 | Bosses: the cheat finisher can break quests, they can be hurt while invulnerable, there is no bar for the guest | `pendiente-jefes.md` | Medium → large |
| 19 | The guest's arrows are not visible on the host's PC | `pendiente-proyectiles.md` | Medium |
| 20 | Riders and mounts not copied | `pendiente-jinetes-animales.md` | Medium → large |
| 21 | Archers and musketeers in towers not copied | `pendiente-jinetes-animales.md` | Large |
| 22 | The copy sinks or floats in one-PC tests | `pendiente-copia-se-hunde.md` | None: it is an effect of the sideways offset; only document it |

---

## Plan, step by step

### Phase 0 — close part 4 (where we left off)

**Step 0.1 — Pending test** (#1). **Done (4 Oct):** damage, death and loot work; stagger works after removing the `AttackTargetAck` from the hits that stagger (`pieza4-tambaleo.md` §8-9). The enemy copies are hit (Ctrl+F7,
`cdmp.ini.prueba-pieza4b`). Check in the log and the capture:
- `VaryStatCheatReq`/`KillCheatReq` with damage matching the table;
- the ATA + AAR + buff batch and the enemy's stagger `StartActionReq` within ≤30 ms;
- death and loot.

If there is no stagger, apply tests R1c/R1d from `pieza4-tambaleo.md` §5.2.

**Step 0.2 — Finisher with experience** (#2). In `guest_hits.cpp`, a lethal hit becomes:
`VaryStatCheatReq` down to 1 HP → `AttackByCollisionReq(E, player, 0xF2EE2678)` (like X2, which gave
reason 8, experience and loot). `KillCheatReq` stays as a fallback at ~500 ms. Never a
`VaryStatCheatReq` that leaves 0 HP.
- **Test:** `AddSubLevelExperienceAck(521, 15)` after the death (test T1 of the report).

### Phase 1 — small fixes, all tested with one PC

**Done on 4 October** (commits `b34f559`, `41dfa34`, `8628d5e`). Live test: horse fine; 8 copies, all
of bandits in combat and none of animals; the originals react to weapon and bow hits
on their copies (arrow `E912512D/769` = 17 HP); on death, the enemies stopped attacking.
- Consequence of 1.3: Ctrl+F7 only copies enemies that are already in combat (when they
  detect you or you hit them). For the guest to see the enemies **before** combat, phase 5
  (zone around the host, hostile enemies by their faction) will send all of them.

Independent of each other; one commit per step.

- **1.1 Keys from any wraparound** (#3): replace the 18 checks `>> 20 == 0xB01/0xA01/0x901` with
  class checks (bits 28-29); remove the hard-coded `0xA0100001`; bound `net_id`; a shared
  counter of instance ids; and a "circuit breaker" that turns off the copies if a game key falls
  into our range.
- **1.2 Arrow damage** (#4): read the 24 B type 1 accessor (projectile key) in
  `player_hits.cpp` and `guest_hits.cpp`. Seeds: 28/17 (player), 24-30 (archers), 6 (muskets).
- **1.3 Enemy selection** (#5): copy only the ones that send `ChangeBattleStateReq` ≠ 0 or
  have a real hit. Reject riders and towers, with the category in the log.
- **1.4 Dead copy** (#6): `copy_state_normal()` checks `Status+0x273`; do not send `PlayerHit`
  or force targets with the copy dead.
- **1.5 Hit timing** (#7): apply the `GuestHit` with the same playback delay as
  the copy, so that the enemy reacts when the sword swing is seen.
- **1.6 Document the sinking** (#22): a note in `PRUEBAS-COOP.md` and in the test `.ini` files
  ("behind you" mode: `offset_m=0.01`, `delay_ms` 1500-3000).

### Phase 2 — protocol 6 and robustness (before playing with the friend)

**Done on 4 October** (commits `b2f2249`, `bc78dd8`, `873c534`): 2.1-2.4 and 2.6. Live test with
`cdmp_peer --loss 5 --jitter 40 --delay 60`: all good, 0 warnings or errors in the log; reconnection (peer
stopped for 10 s) recovered by itself in 1.5 s; moving away from combat removes the copies and they come back when it resumes.
Fixed: a flicker of the copy right after reconnecting. 2.5 (extend `GuestHit`)
moves to phase 3, where it is used. Still missing: the in-game test with `cdmp_peer --loss 5 --jitter 40 --delay 60`
(P0 no. 10 of `pendiente-robustez.md`) and rewriting the `LEEME` for the new zip (phase 4).

All format changes together, to bump `kVersion` only once.

- **2.1 Version** (#8): `kVersion` 6, `kModBuild` = git hash, a warning in the log and on screen if they do not
  match; acknowledge unknown message types and limit resends.
- **2.2 Key translation between PCs** (#9): swap `0xA010F001` ↔ local player in all the
  fields of the enemy messages.
- **2.3 Reliable channel and ordering** (#10): send window, evict partial messages by age,
  in-order delivery of enemy messages, validate sizes before injecting.
- **2.4 Life cycle** (#11): reset the relay when the session or the partner changes; wait until
  in the world before creating copies (with debounce); discard old packets after a
  menu; empty the C>S queue when the world changes.
- **2.5 Extend `GuestHit`**: arrow key, impact point, hit id and clock (needed by
  1.2 and phase 3).
- **2.6 Diagnostics** (#12): rotated logs, configuration dump at startup, a "mark" key
  to synchronize, statistics every 5 s; separate host and guest `.ini` files.
- **Test:** everything with one PC (echo), forcing reconnections and loading screens.

### Phase 3 — combat feels good on both sides

**Status (4 October):**
- 3.1 guest reaction: **done and tested** (commits `8044f68`, `c820aae`, the next one): 3 out of 3 melee
  hits on the copy made the player stagger; arrows, fixed after the test.
- 3.2 guest death: **implemented** (`f3e1284`): 1 HP + own collision, normal death (D1/D2 tested);
  it still needs to be seen with real hits on the copy (enemies rarely attack the copy on one PC).
- 3.3 sharing: in practice it looks fine; what remains is what was done in phase 1 (dead copy) and
  absence (the copy withdraws). No further changes for now.
- 3.4 damage numbers: **dropped** (the game does not show them). Impact effect (`+0x81 = 0`): postponed.
- Extra: automatic reload of a stuck copy; the user key only in aligned fields.

- **3.1 Guest reaction** (#13): the host learns the ATA + AAR + buff batch from enemy hits
  on its own player and sends it in `PlayerHit`; the guest injects it onto themselves
  from the enemy's copy (ids `0x8000E000+n`) and keeps losing health as today.
- **3.2 Guest death** (#13). **Pending decision:** `KillCheatReq` on oneself works,
  but the death telemetry would say "DevCheat" (`pendiente-recompensas-progreso.md`). Before
  implementing it, test with one PC the collision route (`AttackByCollisionReq` on the player
  themselves) and choose.
- **3.3 Enemy sharing** (#14): eligibility in `IsTargetable` (alive, present, ≤40 m); changes
  forced with `CurrentTargetChangedAck` when the guest hits; a filter in `SetCurrentTarget`
  to share; a limit of ~6 enemies on the guest; hysteresis.
- **3.4 Visual response** (#15): `+0x81 = 0` in the injected ATA (impact effect on the
  host); `OnPostBuffDamagedAck` on the guest (damage numbers); a shorter delay for
  the enemy copies.

### Phase 4 — first serious test with the friend (two PCs)

**First test with Shadow (5 Oct), section A:** everything works. Four problems came up, each
with its report:
- **Clothes after dismounting** (`ropa-al-desmontar.md`): **fixed and tested** (protocol 7).
- **The copy runs in place after a menu** (`pausa-copia-congelada.md`): **fixed and tested**
  on one PC; the case of the sender being paused with two PCs is still missing.
- **Small jumps of the host's copy** (`saltos-copia-anfitrion.md`): pending (separate the
  hold-back from the timestamp offset, P1-P5).
- **The time of day does not match** (`hora-del-dia.md`): pending (`SetFieldTimeReq` on the guest).

- New zip with the separate `.ini` files and the new options off by default, except what has already been tested.
- Follow the protocol in `pendiente-robustez.md`: what each one does, what is recorded and what is sent.
- Fix whatever comes up before continuing.

### Phase 5 — a single world of enemies

- **5.1 Remove the guest's own enemies** (#16): first the "log only" mode, then the
  lab mode with a manual zone (tests L0-L4), then the 150 m zone around the
  host. Only the host sends enemies; mirror damage, off.
- **5.2 Rewards for the guest** (#17): finishing off the hidden twin on their server (1 HP +
  collision), once per enemy and only if the guest took part.

### Phase 6 — more enemy types and content

- **6.1 Bosses, safe mode** (#18): list of bosses from the game data; no cheat finishers,
  no injected stagger, never below 1 HP, guest damage halved, no hits
  during cutscenes. Before that, record a real fight against a boss. After that, a boss bar in
  the guest's overlay.
- **6.2 Guest's arrows visible on the host** (#19), behind a switch.
- **6.3 Riders** (#20): first rider and mount separately (visual) and then linked (experimental).
- **6.4 Towers** (#21): later.

### Later / optional

- Make the death from the guest's finisher (collision) look like a sword strike: try other collision
  keys, the stagger before the finisher, or the direction and strength of the impact.

- Bosses in both worlds at the same time (the only way for both to advance the boss quest).
- Snap the copy to the ground with a game raycast, if it turned out to be needed with two PCs.
- Investigate whether the game's internal server could accept a second player (MMO mode).
