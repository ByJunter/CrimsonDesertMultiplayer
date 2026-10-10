CRIMSON DESERT MULTIPLAYER v0.2.0 (ALPHA) - TWO-PLAYER CO-OP
============================================================

Unofficial mod. Not made or endorsed by Pearl Abyss. Use it at your own risk
and always back up your saves first. (Español: ver LEEME.txt)


PLEASE READ: THIS IS AN ALPHA, OR EVEN LESS
------------------------------------------
This mod is a proof of what co-op in Crimson Desert could become, not a
finished multiplayer mode. It has known bugs and unfinished parts.

I will not keep developing this mod. The complexity of this project -
reverse engineering the game's network code, keeping two separate worlds in
sync, shared combat and enemies - is far beyond what I can do with the very
limited free time that I have. (It was developed with the help of Claude, an
AI by Anthropic.)

Also, heads up: someone else is working on a similar project and will be
releasing a much more advanced co-op mod soon. So don't be too bummed out by
how limited this one is, better things are on the way!

This version will be open source: in a few days I will upload all the code
to GitHub under the GPL v3, so that anyone who wants to can continue its
development or use it as the base of their own project. Pull it apart, fix
it, build on it - that is the whole point.


WHAT IT DOES
------------
- Each player plays in THEIR OWN save and sees the other as a character in
  their world: walking, running, jumping, swimming, gliding, ledge climbing,
  wall climbing, attacking, riding a horse, changing gear and using the
  magic rope on trees (the tree bends on the other player's screen too).
- The time of day is the same in both games: the guest takes the host's.
- The other player shows on the minimap and the map (M) as an outlined
  arrow; when they are too far away to be loaded in your world, a map marker
  shows where they are.
- SHARED ENEMIES (experimental). The HOST leads:
  * 150 m ZONE: near the host, the guest stops seeing their own enemies and
    sees copies of the host's enemies instead (up to 48 at a time), with
    their red dot on the minimap. Away from the host (beyond 250 m) the
    guest's own come back.
  * The guest's blows to those copies hurt, stagger and kill the host's real
    enemies, and the guest can start the fight.
  * Those enemies also attack the guest: they lose health, stagger and can
    die (a normal death; they respawn as usual).

KNOWN LIMITATIONS
- Only hostile ENEMIES are shared. Civilians, animals, guards and quest
  characters are each save's own: if your saves are at different points of
  the story, you will see different people.
- Quests and items are not shared. Loot and experience from shared enemies
  go to the host. (There is a VERY experimental, switched-off loot for the
  guest: [coop] guest_loot=1; sometimes it gives nothing.)
- The host's enemies often prefer to attack the host rather than the guest.
- The other player is shown with a small delay (under a second). Arriving in
  an area with many enemies can stutter for a few seconds.
- When the other player gets off their horse, their character reloads in
  1-2 seconds.
- Bosses, mounted enemies and archers in towers are not shared.
- While the host is in a menu (or dead, on the respawn screen) their game is
  paused and the shared enemies stand still.
- Some enemies that crouch or sit in the host's game stand in the guest's.


1. REQUIREMENTS (BOTH PLAYERS)
------------------------------
- Crimson Desert for PC (Steam), version 2.03.02 (main menu, top right:
  "GameVersion : 2.03.02"). With any other version the mod turns itself off
  and the game runs as usual.
- BOTH WITH THE SAME ZIP. If the mod versions differ, a red warning shows at
  the top left ("Different mod versions") and you will not connect.
- Be friends on Steam.
- BACK UP YOUR SAVES first:
    %LOCALAPPDATA%\Pearl Abyss\CD\save
  Copy the whole "save" folder somewhere else.
- DO NOT SAVE WHILE THE OTHER PLAYER IS IN YOUR WORLD. Quit without saving
  or wait until they leave.


2. INSTALL (BOTH)
-----------------
In Steam: right click Crimson Desert > Manage > Browse local files > bin64
folder (where CrimsonDesert.exe is). Copy there:
    winmm.dll   (Ultimate ASI Loader v9.7.4: loads the mod)
    cdmp.asi    (the mod)
    cdmp.ini    (settings; the one in the zip is the GUEST's)

THE HOST: also copy cdmp.ini.anfitrion to bin64 and rename it to cdmp.ini,
replacing the previous one. ("anfitrion" = host, "invitado" = guest.)

Both: open cdmp.ini with Notepad and write your name after player_name=


3. CONNECT
----------
One player is the HOST, the other the GUEST. You must be FRIENDS ON STEAM:
the mod only connects to Steam friends who are playing Crimson Desert. The
connection goes through Steam's servers: no ports to open, it is encrypted,
and neither of you sees the other's IP.

Both cdmp.ini files come ready (role=host in the host's, role=guest in the
guest's). Start the game, both of you. If the host is the only friend of
yours playing Crimson Desert, the guest finds them on their own; otherwise
the guest writes the host's SteamID64 (17 digits) after [steam] peer_steamid=
Windows may ask about the firewall: allowing it helps, but it also works if
you refuse.

Load your saves. When you move, the other player appears in YOUR world where
they are in theirs. If you are far apart you will not see each other: go to
the same place (for example, fast travel to the same point).

ONLY PLAY WITH PEOPLE YOU TRUST: what the other player sends goes into your
game. Someone with a deliberately modified mod could at least crash your game
or spoil the session.


4. KEYS
-------
  F8               show / hide the mod's panel (its language: [overlay]
                   language=en or es in cdmp.ini).
  Ctrl+F7          (HOST ONLY) turn the copies of their enemies in the
                   guest's world on / off.
  Ctrl+Shift+F6    (host only) remove all enemy copies.
  Ctrl+F12         write a numbered mark into cdmp.log, to point out when
                   something odd happened.


5. IF SOMETHING GOES WRONG
--------------------------
- cdmp.log is written next to the game (and the 5 previous sessions in
  cdmp.1.log ... cdmp.5.log). Keep it and say what you saw and when.
- To report a problem in more detail: in cdmp.ini, [sniffer] record=1 saves
  the game's messages to cdmp_packets_* files (about 100 MB per session).
  Set it back to 0 afterwards.
- If the enemy zone causes trouble (enemies missing or not coming back):
  the guest sets [coop] suppress=0 (and sees their own enemies again, plus
  copies of those fighting the host).
- If shared enemies cause trouble: in cdmp.ini, [coop] guest_hits=0 and
  enemies_target_copy=0 (and on the host [dev] enemy_relay=0).
- If the tree rope causes trouble: [dev] rope_bend=0 and
  rope_piece_lookup=0 on both.
- If the horse causes trouble: [coop] horse=0.
- If the time of day causes trouble: [coop] time_sync=0 on both.
- To turn co-op off without uninstalling: [coop] enabled=0.
- The mod learns how much damage your attacks and the enemies' attacks do
  while you play (also alone) and keeps it in cdmp_learnt.txt. Delete it to
  make it learn from scratch.
- To uninstall: delete winmm.dll, cdmp.asi, cdmp.ini and cdmp_learnt.txt
  from bin64.


6. PRIVACY AND SAFETY
---------------------
- The mod collects nothing and sends nothing to anyone but the other
  player. Its logs (cdmp.log, cdmp_learnt.txt) stay on your PC.
- It connects only to Steam friends, through Steam's servers (encrypted;
  neither of you sees the other's IP).
- It changes none of the game's files: it is only loaded next to it. If the
  game crashes with the mod installed, the game's own crash report goes to
  Pearl Abyss like any other crash.


7. LICENSE AND LEGAL NOTICES
----------------------------
Crimson Desert Multiplayer - Copyright (C) 2026 ByJunter
Free software under the GNU General Public License version 3 (LICENSE.txt):
you may use, study, change and share it, but a modified version you publish
must come with its source code under the same license. It comes WITH NO
WARRANTY.

Third-party licenses: see THIRD-PARTY-NOTICES.txt (Ultimate ASI Loader,
Dear ImGui, SafetyHook, Zydis and Zycore).

Crimson Desert and Pearl Abyss are trademarks of Pearl Abyss Corp. This mod
is not affiliated with or endorsed by Pearl Abyss, and it includes no game
files or content: you need your own legal copy of Crimson Desert to use it.
Steam is a trademark of Valve Corporation.
