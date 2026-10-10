# Report: the game crashes when creating the copy of the other player's horse

Crimson Desert, build **1.0.0.2976** (patch 2.03.02).
- PE timestamp `0x6AB28F00`, fixed base `0x140000000`, no ASLR. All addresses are RVAs (`exe+…`).
- The executable uses Denuvo: part of the code is "mutated" in the `.xtls` section.
- The game sends its crashes to Pearl Abyss (sentry/crashpad).

Date: 2026-10-03. This document is self-contained: it is meant to be handed to another person or AI
to ask for ideas.

---

## 1. Context: what the mod does

It is a co-op mod (C++20, `.asi` DLL loaded with Ultimate ASI Loader). It uses hooks with
**safetyhook** and an ImGui overlay on DX12.

**The game is an MMO client/server inside its own process.** The client and a local server
talk to each other with "Troc" messages: `u16 opcode | u8 slot | u16 len | payload`. Names and opcodes
come from the exe's RTTI.
- Hooks on `ClientNet::PostPacket` (`exe+0x35C670`, client→server) and
  `ClientNet::OnRecvPacket` (`exe+0x35CA00`, server→client).
- The mod **injects** server→client messages (Acks) from the `OnRecvPacket` hook, on the client's
  network thread. This way the client "believes" the server is sending it things.

**How the other player appears:**
1. Each PC captures the `ActorLoadDataAck` (opcode 2005) messages of its own character when loading the
   save.
2. It sends them to the other PC.
3. The other PC injects them with **new actor keys**: user `0x9010F001`, player
   `0xA010F001`, body `0xB010F001`. The client creates a "remote player", as in an MMO.
4. That character moves by injecting `MoveActorAck`, `StartActionAck` and `RotateActorAck` built from
   the other player's real `MoveActorReq` / `StartActionReq` / `RotateActorReq`.

**All of this works and has been tested live with a friend over Steam:** movement, climbing,
equipment, minimap icon.

The remote character is in **control mode 2** ("remote"). It is returned by `0x88E8E0(mc)`.
- 1 = local, 2 = remote, 3 = simulated locally with AI, like an NPC.
- It is 2 because `u32 [actor+0x90]` (owner user) ≠ 0.
- In mode 2 the actor simulates nothing: it only interpolates the network buffer and plays the actions that
  reach it through `StartActionAck`.

---

## 2. Goal

Make the other player's **horse** appear too: a copy `0xB010F002` that moves and that the
copy player can ride.

Data of the real horse:
- It is an actor of the user (record type **5 = mount**, key `0xB0100003`).
- It arrives in its own `ActorLoadDataAck` (~8,770 B), 0.1 s after the player, when loading the
  save.
- Its AI runs on the local client (it is "ours").

---

## 3. How the horse copy is created

`ActorLoadDataAck` is a list `[u16 len][record]…`. The handler (`0xCB25A0` → `.xtls
0xA79EA60`) calls `0x8BD530(ClientActorManager, record)` for each record.

**Horse packet: two records.**

| Offset | Field | Real | Copy |
|---|---|---|---|
| `+0x00` | u16 len of record 0 | 9 | 9 |
| `+0x02` | record 0: type 0 (user), u32 key, u32 | `00 90100000 0` | `00 9010F001 0` |
| `+0x0B` | u16 len of record 1 | `0x2235` | `0x2235` |
| `+0x0D` | u8 actor type (1 player, **5 mount**, 7 world…) | 5 | 5 |
| `+0x0E` | u32 key | `0xB0100003` | `0xB010F002` |
| `+0x12` | u32 owner (user) of the record | `0x90100000` | `0x9010F001` |
| `+0x16` | u8 index in the user's child container | 2 | 2 |
| `+0x1F` | first u32 of the data = `actor+0x90` (decides the control mode) | `0x90100000` | `0x9010F001` |

**What changes in the copy:**
- The user, player and horse keys.
- A group member inside the packet.
- The position: it appears 3 times and all 3 are rewritten.

Otherwise it is **byte-for-byte identical** to the real one.

**Creation path with owner ≠ 0:**
1. `0x8AF580` → `0x8ADFE0(owner)`, which requires `u16 [user+0x5E] & 4` (user "logined").
2. Factory `0x4634C0` (`.xtls 0x898D890`).
3. Slot in the user's child container.
4. Unpack `0x17EA310`, which does `actor+0x90 = +0x1F` and calls `vf11`/`vf13`/`vf14` of each
   component.

With owner = 0 the factory `0x4631C0` is used (like the player's body, type 7).

**Before the horse, the real server sends** a "preamble" of Acks: `VaryHPAck`, `ApplyBuffAck`
×n, `RefreshMercenaryDataAck` (11.6 KB), `JoinPartyAck`, `SetMinStatRateAck`… We do **not**
replay it. Static analysis says none of that touches actions or animation layers.

---

## 4. Attempts and results (all live, a single PC, "mirror" over loopback)

| # | What was done | Result |
|---|---|---|
| A | Load with owner `0x9010F001`, in the same batch as the copy player | **Not created**, no error and no crash. Hypothesis: the copy user did not yet have `+0x5E & 4` |
| B | Load **without owner** (`+0x12 = 0`, `+0x1F` = `0x9010F001`) | **Crash** at `exe+0x1F63FE6` |
| C | With owner, injected **3 s after** the player | **Crash** at `exe+0x1F63FE6` (the same one) |
| D | C + **R1**: mid-hook at the start of `0x1F63F70` that returns 0 if the layer's "playback" is null. + **R2**: hold back the load until the sender horse's first `StartActionReq` and inject its `StartActionAck` in the same batch, right after it | R1 fired 1 time. Then **crash** at `exe+0x232D5FF` |
| E | D + mid-hook at the start of `0x2329700` that skips the whole function for our copies if a layer has a graph but no playback | The skip fired 1 time. Then **crash** at `exe+0x925F0E` |

In B to E the crash comes **~0.23 s after injecting the horse load**:
- D: load at 22:05:44.838, crash at 45.073.
- E: load at 22:32:47.873, crash at 48.108.

The sniffer records the delivery of the load, but **not** that of the `StartActionAck`. It is not known whether the
Ack was applied or was waiting in the client's retry queue, which holds the Acks for actors
that are "not ready yet" (`0x8B53C0`).

---

## 5. The crashes in detail

All three are null pointer reads, on a **worker thread** that updates actors in
parallel. The common chain is:

```
thread loop 0x1414DE0 → ParallelTaskWorker::vf3 0x13FB350 → "update actor" task 0x8B56D0
  → update of the ClientCharacterControlActorComponent (CC) 0x85FBA0 → …
```

### 5.1 The CC's "action layers"

- `CC = [[actor+0x68]+0x40]`. It has 2 layers: `[CC+0x88]` (layer 0, main) and `[CC+0x90]`
  (layer 1).
- Each layer is a 0x118 B object (constructor `0x2435B70`, all zeroed):

| Field | Meaning (inferred) |
|---|---|
| `+0x00`, `+0x08`, `+0x10` | current action: `+0x08` data (speed at `+0x27C`), `+0x10` node (state at `+0xEC`, flags at `+0x108`/`+0x10A`) |
| `+0x18` | playback of the current action |
| `+0x20` / `+0x24` | previous / current time |
| `+0x100` | ? (read by `0x925CE0`) |
| `+0x110` | the layer's own action graph |

**The horse copy has the main layer with a graph (`+0x110 ≠ 0`) but no action
(`+0x10`, `+0x18` null).**
- The real horse is never in this state: its local AI starts the idle action `0x27FE18E9`
  0.31–0.41 s after loading. This is seen in 6 out of 6 captures.
- The copy player (mode 2) is also born without an action, but **does not crash**. Presumably its layer
  has no graph of its own (`+0x110 = 0`) until the first action, so those paths do not
  run.

### 5.2 Crash 1: `exe+0x1F63FE6` (attempts B and C)

- Function `0x1F63F70`: advances the time of a layer's action.
  - `rcx = &layer+0x18`; it does `r = *rcx` (null) and then `mov rdx,[rax+0x10]`, which reads `0x10`.
- It is called by `0x2329700` (advances the 2 layers, from `0x863E80` at `0x8641C6`).
  - `0x2329700` only checks `layer+0x110 ≠ 0`, `dt > 0` and `rate > 0`; **it does not check `+0x18`**.
  - Almost all other consumers of the field do check it.
- Stack: `0x13FB350 → 0x8B56D0 → 0x85FBA0 → 0x863E80 → 0x2329700 (layer 0) → 0x1F63F70`.
- The `dt` of the actor that crashed was 6.485 s. It matches the time since the **real** horse
  loaded, not the copy, so it is not 100 % proven that the actor was the copy.

### 5.3 Crash 2: `exe+0x232D5FF` (attempt D, with R1 active)

- Inside `0x2329700` → `0x232D2A0` → `0x232D5C0`.
- `0x232D5C0` does `rax = [r8+0x10]` and then `movzx eax, word [rax+0xE4]`, with `rax = 0`.
  - `r8` is a local structure of the caller (`[rsp+0x48]` of `0x232D2A0`), a copy of fields of the
    layer.
  - The `+0x10` is the current action node, null.
- Stack (return addresses): `0x232D5B2`, `0x8641CB` (the call to `0x2329700`),
  `0x860115`, `0x8B5766`, `0x13FB680`.

### 5.4 Crash 3: `exe+0x925F0E` (attempt E, with all of `0x2329700` skipped for the copy)

- It is called by the CC update `0x85FBA0` at `0x860154`, already **outside** `0x863E80`:
  ```
  rcx = [[[CC+8]+0x68]+0x58]+0x6E8]   ; CC+8 = actor; component slot 0x58 = AI (Ai)
  call 0x925CE0
  ```
- `0x925CE0` walks the 2 layers (`[[[actor+0x68]+0x40]+0x88 + 8·i]`):
  - `r11 = [layer+0x10]` (current action node) and `r10 = r11`.
  - Non-null is only checked to read `edi = [r11+0x18]`.
  - Afterwards, if the "vehicle" branch is active, it reads `movzx eax, byte [r10+0x108]` **without
    checking**, and crashes.
- The "vehicle" branch is `[rbp+0x78] = 1`, which holds when the Vehicle component
  `[[actor+0x68]+0x48]` has `+0xEB ≠ 0`, `+0x48 ≠ 0` and `+0xEA ≠ 0`. That is why it only affects
  mounts.
- Stack: `0x925F0E` ← `0x860159` (in `0x85FBA0`) ← `0x8B5766` ← `0x13FB680`.

**Conclusion:** many functions in a mount's update assume that **there is always
an action in the main layer from the first frame**. Patching read by read never
ends: each guard exposes the next one.

---

## 6. Current hypotheses

1. **Main:** a mount actor in mode 2 (remote, no local AI) never receives its first action
   in time.
   - The `StartActionAck` injected right after the load is not applied before the CC's first
     tick. It most likely goes to the retry queue because the actor is not "ready" yet.
   - The horse's CC starts updating as soon as the layer's graph exists.
2. **Alternative (H2):** the actor that crashes is the **real** horse `0xB0100003`, which the
   copy "wakes up" or corrupts.
   - They share mercenary no. 12 and the group, and the copy is ~2.5–12 m away.
   - The `dt` of crash 1 supports it.
   - In crashes 2 and 3 it has not been checked which key the actor had; the crashpad dump only
     contains stacks, not the heap.

---

## 7. Untested ideas (to be assessed)

1. **Skip the whole CC update (`0x85FBA0`) of the horse copy** while
   `[layer0+0x10] == 0`.
   - More general than the previous patches.
   - Risk: leaving other subsystems half-done. It is also unknown whether the action would ever be applied
     with the CC stopped.
2. **Give it an action on the game thread, synchronously, right when it is created**, before its
   first tick:
   - by calling the internal function used by the `StartActionAck` handler, or the one used by the
     local AI for the idle `0x27FE18E9`;
   - or by hooking the end of the actor's creation.
3. **Create the copy without its own graph in the layer** (`+0x110 = 0`), like the copy player, so
   that those paths do not run.
   - It would be necessary to find where `+0x110` comes from: record data, character type or
     component.
4. **Make the horse copy "simulated locally" (mode 3)**, like world horses, and
   filter its outgoing Reqs.
   - Its AI would start it correctly.
   - But it ignores the network buffer, so it would have to be moved some other way (teleports,
     `MoveForceAck`), and it would send Reqs with its key to the local server.
5. **Use a "world" mount** (the captures contain 16 of type 5 with owner 0 and local AI) instead
   of the user's horse.
6. **Replay the server preamble** (`RefreshMercenaryDataAck`, `JoinPartyAck`, buffs…)
   before the load. Static analysis says it does not touch actions, but this is not tested.
7. **Find out exactly which actor crashes**: a read-only mid-hook at `0x85FBA0` that
   logs `[[CC+8]+0x60]` (key) and the state of the layers on every frame for 1 s after the
   load. This settles H1 versus H2.

---

## 8. Specific questions

1. What is the "correct" way for a network-created mount actor to have an initial action
   before its first update tick?
2. In Pearl Abyss engines (Black Desert uses the same code family, with Troc
   messages, `ClientCharacterControlActorComponent`…) is there a "post-load" step that sets the initial
   action of a remote actor, and which message triggers it?
3. Is there a better pattern than patching null reads one by one?

---

## 9. Project constraints

- There is only one PC for testing. The "other player" is a loopback echo (`cdmp_peer`).
- Every crash is sent to Pearl Abyss. The mod's memory reads use SEH/`VirtualQuery`.
- Hooks only at the start of functions (inline) or mid-hooks with safetyhook. No patches in the middle
  of a function without `create_mid`.
- Do not identify actors across PCs by memory address; keys are used.
- Before each test the saves are copied, and the game is never saved with the copy loaded.

---

## 10. Evidence (in the project)

| File | What it is |
|---|---|
| `captures/horse_crash_210557.dmp`, `captures/cdmp_210510_horse_crash.log` | Attempt C (crash 1) |
| `captures/horse_crash_220545.dmp`, `captures/cdmp_220545_horse_crash2.log` | Attempt D (crash 2) |
| `captures/horse_crash_223248.dmp`, `captures/cdmp_223248_horse_crash3.log` | Attempt E (crash 3) |
| `docs/research/caballo-coop.md` | Full analysis: horse messages, load format (§8), crash 1 and recipes (§9) |
| `docs/research/movimiento-remoto-cliente.md` | Control modes and movement of the remote actor |
| `src/mod/net/remote_puppet.cpp` | `spawn_horse`, `horse_event`, guards `action_layer_advance` and `action_layers_advance` |
| `src/mod/game/offsets.h` | Addresses used, with their explanation |
| `build/research/horse3/mdmp.py` | Minidump reader (exception, registers, stack) |

The horse is **disabled by default** (`[coop] horse=0`). With it disabled, the
co-op works without crashes.

---

## 11. Update: crash solved (2026-10-03, 22:58)

The external review was followed. Read-only diagnostics (`src/mod/net/horse_diag.cpp`) with
mid-hooks at `0x85FBA0` (CC update), `0x8646D0` (restoring actions after loading
resources) and `0x2329D70` (pending actions). Log: `captures/cdmp_horse_diag_ok.log`.

**Results:**
- **H1 confirmed.** The actor at risk is the copy `0xB010F002`, not the real horse.
- **Restore byte `[[CC+0x40]+0x30]`:** it is **1** on the real horse and **0** on the copy.
  That is why `0x8646D0` restores no action for it. Restore data of the copy:
  `+0x30 = 00`, `+0xF8 = 0`.
- **The `StartActionAck` is delivered**, about 0.6 s after the load.
  - At first it stays pending (`CC+0xA8 = 1`).
  - It is applied about 0.7 s later: layer 0 with `+0x10` and `+0x18` non-null.
- **Containment:** skip `0x85FBA0` and `0x860D30` for the copy while layer 0 has a graph without an
  action. Skipping **1** update was enough.
  - No crashes: the horse copy appears, moves, and the copy player rides it.

**Next problem:** when dismounting, the copy rider freezes mid-animation.
- Cause: there is no real rider↔horse link, so `RideOff::Enter` fails.
- Fix: when dismounting, reload only the rider and leave the horse copy in the world.

**Pending (improvement):** put the restore byte and the action data into the load
packet, so that the engine restores the action through its native path and the containment is no longer needed.
