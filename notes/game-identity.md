# RIP 3: The Last Hero — game identity

## Observed
- Store: Steam, RIP - Trilogy (app 2540). OS: Windows.
- `RIP3/RIP3.exe`: 678 912 bytes, SHA-256 `C4305AA61DBC06E72776E936943300DDDF3A6D82176CD393C4F50D04C9D925D2`. No version resource; version = unknown, identify by hash.
- Packed with PECompact 2 (`PEC2` marker). Sections: `.text` (vsize 2 572 288, raw 666 624), `.rsrc`.
- Neighbours: `HawkNL.dll` (network), `audiere.dll` (audio), `data.pak` (24 MB), levels `1-100.rl2`, `m1-m10.rl2`.
- `config.ini` contains `ServerName=RIP3_SERVER`.
- Main menu has "Cooperative Mode": local 2-player, each player picks a character and a name.
- Not listed in coop-anything DIRECTORY.md.

## Reverse engineering (dump of the Steam-launched process, local/dump)
- The exe only starts when launched by Steam (otherwise "Failed to find Steam"). The dump is taken read-only from the running process: `scripts/dump.ps1`, run with 32-bit PowerShell.
- Ghidra headless: `scripts/ghidra/run_headless.cmd` via `subst R:` (the bat launcher breaks on "(x86)").
- Main menu (FUN_0040c210): SINGLE PLAYER, COOPERATIVE MODE, MULTIPLAYER, LEGEND, PROFILES, OPTIONS... The MULTIPLAYER submenu has SERVER / CLIENT, a server list with REFRESH, CREATE SERVER, LAUNCH.
- Network: HawkNL (nlListen/nlConnect/nlRead/nlWrite/nlPollGroup); the log strings are "[NETWORK] Server accepted connection #%d", "Trying to connect to %s".
- Level load (FUN_004084f0): mode byte at game+0xE861. 0 or 1 -> `Levels/%d.rl2` (story: single/coop), otherwise -> `Levels/m%d.rl2` (multiplayer maps).

- MULTIPLAYER in the menu (owner's report): SERVER = choose a map from the pool, frag limit, time limit; CLIENT = list of available servers. I.e. deathmatch.

## Network protocol (local/re/net_decompiled.c)
- Transport object: server up to 5 clients (DAT_004fc618 < 5). Packets are up to 0x101 bytes, the first byte is the type:
  - 1: server announcement with its name (polled by the client, filling the server list; 0x100 bytes of name/data)
  - 2: server -> client "accepted", byte = client number
  - 4: ping/time (nlTime), 5 x u16 latency per slot
  - 5: game data (<=0x100 bytes), put into a queue (list at +0x18/+0x1C). Send: FUN_00440950 (client->server), FUN_004406f0 (server->client N)
- Game serializers (nlSwapf/nlSwaps = byte order):
  - FUN_00446bf0 / FUN_00447580: 0x19 bytes, player state (x, y, angle, health(?), weapon, 4 state bits, id)
  - FUN_00455890 / FUN_00455930: 0x12 bytes, projectile/object (x, y, angle, type, owner)
  - FUN_004467f0 / FUN_00446870: 0xC bytes, small object (x, y, id)
  - FUN_00447f50: 0x20 bytes (unclear)
  - FUN_00404b50 -> FUN_004048e0: 0x10 bytes, effect/event
- FUN_0041d490 (1200 lines): most likely the network tick of the game (sending/dispatching type 5). FUN_0042ee60: game logic calling into the network part.
- Not seen yet: serializing monsters/AI and level scripts — expected, there are none in deathmatch.
- The game functions take a float dt (FUN_00440d10(param_2)) — probably a variable timestep; this matters for lockstep.

## Guesses (unverified)
- Network MULTIPLAYER = arena mode on m1-m10 only, not the story; the local coop (mode 1) has no networking.
- `m1-m10.rl2` may be multiplayer/deathmatch maps.
- HawkNL + ServerName suggest some socket-based network mode exists.

## Goal
Online co-op with an invite code: the friend joins on their own PC with their own copy, picks a character and name, and plays the story levels.

## Workspace
- `game/` — copy of the install (hash matches the original). The original install is never touched.
- `tools/x64dbg` — snapshot 2026.05.27, zip sha256 d41966df…7697 (GitHub digest matched).
- Ghidra: the owner is downloading it. JDK 21 is still needed.
