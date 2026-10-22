# Stage 1: is lockstep feasible

Sources: local/re/lri_decompiled.c, local/re/want_decompiled.c (dump of the Steam process, Ghidra 12.1.4).

## Observed in code

### Game frame
- `FUN_00457110`: WinMain message loop. While there are no messages and the game is active (`DAT_0051aab5`), it calls one frame:
- `FUN_00412880(game)` — **one game frame**, strict order:
  1. `FUN_0043efb0(timer)` — frame time measurement (speedfactor)
  2. `FUN_004155b0` — input/camera/menu
  3. `FUN_0042ee60` — game logic (calls rand, reads keys)
  4. `FUN_0041d490` — network tick (deathmatch)
  5. `FUN_0043ac00` — rendering (probably)

### Time — variable timestep
- `FUN_0043efb0`: QueryPerformanceCounter, frame time averaged over the last 4 frames, `speedfactor = avg * k`; spins in a loop while the FPS is above the cap. Result is `timer[8]` (speedfactor), fed into game logic.
- Consequence: without a hook, two PCs compute different steps -> they diverge.

### RNG
- One generator: CRT `_rand` (`004a1007`, MSVC LCG 0x343FD/0x269EC3, `_holdrand` in per-thread data). About 30 call sites, including monster AI and effects.
- Seeding: `srand(timeGetTime())` (`004a0ffa`) once during initialization in `FUN_004138a0`.

### Input
- DirectInput8 (`FUN_004127c0` -> `FUN_0043e000`). Input object: keyboard (device at +0x8, 256-byte state at +0x7, previous frame at +0x107), mouse (+0x210), 2 joysticks (+0x464, step 0x1B8).
- `FUN_0043e380(key)` = key held, `FUN_0043e3a0(key)` = key just pressed. Read in `0042ee60`, `00416810`, `00417cd0`, `00436e40`.
- Not yet found: which keys/joystick belong to player 2 (the controls table from the CONTROLS menu).

## Verdict (derivation, not verified by test)
Lockstep **is technically feasible**: all randomness comes from one rand(), time comes from one function, and input from one DirectInput object. Hook points:
1. `FUN_0043efb0` -> fixed speedfactor (fixed step, e.g. 60 Hz) + frame pacing ourselves.
2. `_holdrand` -> the same seed on both sides when the co-op level starts (the host sends it).
3. After the DirectInput poll -> replace the keyboard/mouse/joystick state with the identical "merged" frame input: player 1 controls from the host, player 2 controls from the client.
4. Wrap `FUN_00412880` -> execute frame N only when the input of both for frame N has arrived.

## Risks / unknowns
- Unknown: whether the logic uses time anywhere besides speedfactor (e.g. timeGetTime directly in AI). Only the init call to timeGetTime was found — a good sign, but it needs a test.
- Mouse aiming for player 1: the mouse state must also be part of the shared input.
- x87 floating point: same exe on both sides -> expected to match, verified by a desync test.
- rand() is also called from rendering/effects -> if a call count depends on FPS/rendering, the sequence diverges. Needs a test (hash comparison).

## Stage 2 status (2026-10-21, paused at owner's request)
- build/rip3hook.dll + build/inject.exe (src/), built with Zig 0.17.0 (`zig cc -target x86-windows-gnu`). All hooks install (rand/srand, frame, timer, poll).
- **Bug in the DLL:** with the DLL loaded (recording F9), the game crashes on the first shot/step in a co-op level: AV at 0x0043B0CB (FUN_0043b0c0, sound slot = NULL; the slot index is computed, e.g. `idx+5` at 0x0041B033). Without the DLL the owner completed level 1 in co-op with no crashes -> the cause is ours.
  - In the menu with the DLL: sound enabled=1, slots 0..79 loaded.
  - Final update (2026-10-21): the AV at 0x0043B0CB is a GAME bug when the audio device disappears (the owner unplugged headphones -> sound slots invalid -> crash on the first sound). It also happened without any mod. The record-mode crash was most likely this, not the DLL. Test needs to be redone.
  - Update: the owner completed level 1 in the same process (pid 18196) where rip3hook.dll was loaded in IDLE mode (no F9) — no crash. So the cause is in what is active only in RECORD mode: the timer override (t[2..9]) or the fixed seed. my_rand in idle mode is fine.
  - Earlier suspects: my_rand (global seed instead of per-thread _holdrand), the timer override (t[7..9]), recording in my_poll. Check by turning hooks off one at a time.
- The 11:31 crash at the same address happened without the DLL — circumstances unknown (possibly a direct launch of the exe without Steam).
- Russian language: present in the exe (alt string table), DAT_004fc60c is forced to 0 in FUN_004138a0.
  - build/rip3ru.dll (src/rip3ru.c) + `inject.exe --wait`: waits for the game window, then sets the byte to 1. Code patching is NOT allowed: PECompact checks the code after unpacking -> "Application corrupt" (first attempt NOPed the instruction).
  - Result (owner, 2026-10-21): no crashes, Cyrillic renders, but only the MAIN MENU and PAUSE are translated; all other text is English (no alt strings in the exe or it lives in data.pak/textures).

## Stage 2 result (2026-10-21): DETERMINISTIC
- Recording try2: 5042 frames (menu -> 2 players -> level, P1 mouse, P2 keyboard scheme 1: WASD, H/K rotate, J fire, U alt).
- Replay needs both DirectInput state AND window messages (menu clicks/P1 fire are WM_LBUTTONDOWN -> fire flags game+0xFB25..).
- Bug fixed: the input mask treated dword input+0x0C (01 01 01 00 = keyboard present/valid/acquired flags; FUN_0043e380 returns 0 when kbd+5==0) as a pointer. Pointer test now needs v >= 0x400000; mask for try2 regenerated offline (constant pointer-like dwords only).
- Replay (stopped by owner at frame 4294): RNG seed and rand call count identical on ALL frames; full state hash identical on every frame from 2807 on (earlier mismatches = menu/loading noise). Owner: "works 1:1 as I played".
- => Lockstep is viable: fixed 60 Hz step + own rand seed + per-frame input object (masked) + window messages.

## Next step (old)
Desync test without the network: two game instances (or one with record/replay) + an injected DLL that fixes the step and the seed, records input and a hash of the state per frame. Same input -> compare hashes.
