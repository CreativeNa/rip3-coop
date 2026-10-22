# Stage 3: online co-op (lockstep over direct UDP)

Owner chose: direct UDP + invite codes (no Steamworks: the game has no steam_api.dll, Steam is only a DRM launch check).

## Design
- `src/netproto.h`: wire format (shared). Host input = masked DirectInput object (0x7D4) + window messages; client input = 256-byte keyboard (P2 keys, scheme 1: WASD move, H/K rotate, J fire, U alt). Merge = host input, keyboards OR'ed (same as two people on one keyboard). kbd+5 (input+0x0D) forced to 1. The previous-key buffer (input+0x10F..0x20E) is left to the game.
- Frame N runs only when both inputs for N are known; each side samples during frame N for N+delay. Delay from RTT (3..15 frames).
- Session start (host F8 in the main menu): START in chunks: seed, delay, mask, host base input, profiles (game+0xE8E0, 0x768 = save.dat contents), profile idx +0xF048, mouse sens +0x155C0, blood +0xB21.
- Client: host profiles applied in memory; save goes to `coop_save.dat` next to the DLL (the push of "save.dat" in FUN_004076D0 is patched); own settings restored on WM_DESTROY (config.ini is written on exit, FUN_004017b0).
- Desync check: (seed, rand calls) per frame exchanged in INPUT packets.
- The main loop runs frames only while DAT_0051AAB5 is set (window active): forced to 1 during a session.
- `src/rip3net.c` -> build/rip3net.dll: written, NOT built or tested yet. Talks to the launcher over 127.0.0.1 (launcher 27600, DLL 27601).

## Status (2026-10-22)
- Built and tested: bot-host replay (client side) 5042/5042 frames matched (before the mouse rework); bot-friend (host side) works, owner confirmed P1 and P2 behave correctly.
- Mouse: the mouse object accumulates (cursor = old + delta, clamped, moved by the game), so it is NOT copied as state. Raw buffered GetDeviceData events travel (np_msg with msg 0xFFFF), served to the game's own poll through a vtable hook on the mouse device. Only keyboard keys (0x0F..0x10E) and joystick blocks (0x2B0..0x620) are copied as state; 0x620.. is derived game state.
- The bot-host recording (try2) has no raw mouse events, so test 4 is outdated until re-recorded.
- Missing audio device crash fixed in rip3video.dll (safe FUN_0043b0c0 + silent stream in empty slots).
- In-game panel (F7, rip3net.dll overlay window) drives rip3coop.exe --service <pid> via PK_CMD/PK_INFO.
- 2x enemy HP/damage in 2-player mode is the game's own behaviour (owner checked without the mod).
- Bundle: dist/rip3-coop.zip. Not yet tested online with a real friend.

## Added 2026-10-22 (not tested with a second game yet)
- Menu button MULTIPLAYER (game+0x15418, click check at 0x41029C) opens the co-op screen (overlay, game cursor hits).
- Codes: 9 chars = public IPv4 + 12-bit check (fixed UDP port 27605); 13 chars when the router changed the port. Same public IP -> LAN broadcast.
- Shared UI: game+0xB68 != 2 (menus) or game+0xB80 == 3 (pause): the friend's mouse events and window messages are merged too. Esc from either side always. Overlay shows who paused.
- Resync at every level load (FUN_004084f0, hooked at all call sites): RNG reseeded alike; host sends players 0-1 (game+0x1738, 2x0x22DC) + profiles; the friend waits inside the load and applies non-pointer dwords.
- Friend's progress: on exit the co-op profile goes into the friend's save.dat as "КООП <name>" (backup save.dat.coopbak).

## TODO (old plan, done)
1. `src/rip3coop.exe` launcher (console, Russian, UTF-8):
   - menu: host / join / bot-friend test / bot-host test
   - UDP socket, STUN (stun.l.google.com:19302) for the public ip:port, LAN ip
   - code = base32(ver, pub ip, port, lan ip, port, nonce), copied to the clipboard; exchange both ways
   - HELLO punching, PING/PONG RTT
   - forwards packets 0x10..0x7F between the peer and the DLL; LINK every 250 ms; prints STATUS
2. Bots:
   - bot-host replays local/runs/try2 (input.bin, mask.bin, msgs.bin, seed 0x1234567) to the client DLL and compares the client's checks with hash_record.txt
   - bot-friend sends scripted P2 keys (with simulated latency/loss) to test the host
3. Build (add to scripts/build.ps1: rip3net.dll -lws2_32, rip3coop.exe -lws2_32 -luser32), add rip3net.dll to start_rip3.cmd.
4. Package a bundle for the friend; real online test.

- Faster startup (rip3video.dll, FastLoad=1): XUnzip name lookup (0x443780) replaced by a name index built once (1773 files); libpng png_do_read_interlace (0x473BF6) and png_combine_row (0x4739E0) replaced (both __stdcall!); zip read/seek/tell hooked too (data.pak is already in memory, so that part changes nothing). Window to menu 6.5 s -> 3.2 s; owner checked images, a match and the help screen. Profiler: Profile=1 in rip3video.ini.
- 2026-10-22: boss research dumps, laser stub and the ai: log removed. Status line in a session: ping, input delay, % of the last second spent waiting for the friend; green/yellow/red. XInput pad (pad_keys in my_poll) presses player 2's scheme-1 keys, right stick turns towards its direction; active alone or as the friend, never on the host. Not tested with a real pad yet.
- Widescreen with a wider view (owner's item 11): level load (FUN_004084f0) sets the arena size 0x4F1270/74 and centre 0x4F1278/7C: 1024x768 (exactly one screen, no scrolling) unless game+0xE861 == 2, then 2048x1536 with a moving camera. So a wider view shows nothing more on normal levels; only the big-arena mode could gain. Screen-size constants: local/re/screen_const.txt (scripts/ghidra/FindScreenConst.java), drawing FUN_004316b0.
- Bot test AI (rip3net.dll ai_keys, host only): nearest-enemy aim via angle player+0x22B0 (radians, 0 = up, clockwise), J/U fire, weapons 1-3 cycle, dodge enemy projectiles (owner -1) by closest approach, keep off melee range, follow host, unstuck. Owner test: hits well, dodges rockets, survived a boss; walked into flamethrower once (near-hazard push added after).
