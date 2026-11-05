// rip3net.dll - online co-op for RIP 3 (exe SHA-256 C4305AA6...D925D2).
// Lockstep: both games run identical fixed 60 Hz frames; only inputs travel. The game
// talks to its rip3coop.exe launcher over 127.0.0.1 (netproto.h); the launcher holds the
// internet socket. Proven deterministic by the record/replay probe (rip3hook.c,
// notes/stage1-lockstep.md).
// Without a launcher the DLL stays idle and the game behaves as usual.
// Host: F8 in the main menu starts / stops the session.
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "netproto.h"

#define ADDR_RAND        0x004A1007u
#define ADDR_SRAND       0x004A0FFAu
#define ADDR_FRAME       0x00412880u  /* fastcall(game) - one frame */
#define ADDR_MAINLOOP    0x00457110u  /* calls FRAME */
#define ADDR_TIMER       0x0043EFB0u  /* thiscall(timer, int cap) */
#define ADDR_INPUTSTEP   0x004155B0u  /* calls INPUT_POLL first */
#define ADDR_INPUT_POLL  0x0043EEF0u  /* thiscall(input) */
#define ADDR_TIMER_K     0x004BA998u  /* double used to average 4 frame times */
#define ADDR_WNDPROC     0x00457000u  /* calls MSGPROC for every message */
#define ADDR_MSGPROC     0x0041D220u  /* thiscall(game, msg, wparam, lparam) */
#define ADDR_ACTIVE      0x0051AAB5u  /* main loop runs frames only while this is set */
#define ADDR_SAVE_FN     0x004076D0u  /* writes the profiles to save.dat */
#define ADDR_SAVE_NAME   0x004B9A74u  /* "save.dat" */
#define GAME             ((uint8_t *)0x00503B50u)

#define FIXED_HZ         60.0
#define PEER_TIMEOUT_MS  30000
#define MIN_DELAY        3
#define MAX_DELAY        15

enum { S_IDLE, S_STARTING, S_RUNNING };

static SOCKET g_sock = INVALID_SOCKET;
static struct sockaddr_in g_launcher;
static int g_link_ok, g_role, g_peer_connected;
static uint16_t g_rtt;

static int g_state = S_IDLE;
static uint32_t g_session, g_rx_session, g_rx_chunks, g_acked_chunks;
static np_start g_start;
static uint8_t g_mask[INPUT_OBJ_SIZE];          /* 1 = host data byte */
static uint8_t g_host_base[INPUT_OBJ_SIZE], g_zero[INPUT_OBJ_SIZE];
static np_frame g_local[NP_RING], g_remote[NP_RING];
static uint32_t g_have_local, g_have_remote, g_peer_ack, g_frame, g_delay;
typedef struct { uint32_t frame, seed; uint16_t calls; uint8_t valid; } check_slot;
static check_slot g_my_checks[NP_RING], g_peer_checks[NP_RING];
static int g_desync_reported;
static int g_link_lost;                  /* waiting for the friend after a short outage */
static np_msg g_pending[256];
static int g_npending;
static int g_in_dispatch;
static uint32_t g_resync_id, g_level_loads;
static uint64_t g_resync_have;  /* resync chunks: friend - received, host - acked */
static int g_nmouse;             /* raw mouse events collected for our next frame */
static DWORD g_last_peer_tick, g_last_send_tick, g_last_chunk_tick, g_last_status_tick;
static uint32_t g_stalls;
static uint32_t g_wait_pct;      /* % of the last second spent waiting for the friend */

static int g_saved_settings, g_save_redirected;
static uint8_t g_own_profile_idx, g_own_blood;
static int32_t g_own_mouse_sens;

static uint32_t g_seed = 1, g_rand_calls;
static void *g_input_obj;
static LARGE_INTEGER g_qpf, g_last_frame;
static char g_dir[MAX_PATH], g_coop_save[MAX_PATH];
static int g_lang_ru = 1;                /* game language, read from build\rip3mod.ini */
static FILE *g_log;

typedef void(__fastcall *frame_fn)(void *game);
typedef void(__fastcall *timer_fn)(float *timer, void *edx, int cap);
typedef void(__fastcall *poll_fn)(void *input);
typedef int(__fastcall *msgproc_fn)(void *game, void *edx, uint32_t msg, uint32_t wparam, uint32_t lparam);
static msgproc_fn orig_msgproc = (msgproc_fn)ADDR_MSGPROC;
static frame_fn orig_frame = (frame_fn)ADDR_FRAME;
static timer_fn orig_timer = (timer_fn)ADDR_TIMER;
static poll_fn orig_poll = (poll_fn)ADDR_INPUT_POLL;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void language_path(char *out, size_t n) {
    snprintf(out, n, "%s\\rip3mod.ini", g_dir);
}

static void load_language(void) {
    char path[MAX_PATH], value[16];
    language_path(path, sizeof path);
    GetPrivateProfileStringA("General", "Language", "ru", value, sizeof value, path);
    g_lang_ru = _stricmp(value, "en") != 0 && _stricmp(value, "english") != 0;
    logf_("game language: %s", g_lang_ru ? "ru" : "en");
}

static int save_language(void) {
    char path[MAX_PATH];
    language_path(path, sizeof path);
    int ok = WritePrivateProfileStringA("General", "Language", g_lang_ru ? "ru" : "en", path) != 0;
    logf_("game language set to %s (%s)", g_lang_ru ? "ru" : "en", ok ? "restarting" : "write failed");
    return ok;
}

/* The language table is applied at injection, so relaunch the game through the same
   launcher. It waits for this process to exit and starts a fresh one. */
static void restart_game(void) {
    char root[MAX_PATH], script[MAX_PATH], ps[MAX_PATH], cmd[MAX_PATH * 2];
    snprintf(root, sizeof root, "%s\\..", g_dir);
    GetFullPathNameA(root, sizeof root, root, NULL);
    snprintf(script, sizeof script, "%s\\start_rip3.ps1", root);
    if (GetFileAttributesA(script) == INVALID_FILE_ATTRIBUTES) {
        logf_("restart: %s not found", script);
        return;
    }
    const char *windir = getenv("SystemRoot");
    snprintf(ps, sizeof ps, "%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
             windir ? windir : "C:\\Windows");
    snprintf(cmd, sizeof cmd, "\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps, script);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, root, &si, &pi)) {
        logf_("restart: CreateProcess failed (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf_("restart: launcher started, terminating the game");
    /* ExitProcess can linger in DLL detach; Steam then keeps App Running and
       refuses the relaunch (AppError_16). A hard terminate makes it clear fast. */
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

/* ---- patching ---- */

static int write_code(uint32_t at, const void *bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void *)at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)at, bytes, n);
    VirtualProtect((void *)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, n);
    return 1;
}

static int jmp_to(uint32_t from, void *to) {
    uint8_t b[5] = { 0xE9 };
    int32_t rel = (int32_t)((uint32_t)to - (from + 5));
    memcpy(b + 1, &rel, 4);
    return write_code(from, b, 5);
}

/* Redirects every `call target` (E8 rel32) inside [fn, fn+len) to `to`. */
static int redirect_calls(uint32_t fn, uint32_t len, uint32_t target, void *to) {
    int n = 0;
    for (uint32_t p = fn; p + 5 <= fn + len; p++) {
        if (*(uint8_t *)p != 0xE8) continue;
        int32_t rel = *(int32_t *)(p + 1);
        if (p + 5 + rel != target) continue;
        int32_t nrel = (int32_t)((uint32_t)to - (p + 5));
        if (write_code(p + 1, &nrel, 4)) n++;
    }
    return n;
}

/* ---- link to the launcher ---- */

static void send_raw(const void *p, int n) {
    if (g_link_ok && g_sock != INVALID_SOCKET)
        sendto(g_sock, (const char *)p, n, 0, (struct sockaddr *)&g_launcher, sizeof g_launcher);
}

/* status line for the launcher console (UTF-8) and the log */
static char g_status_text[200];   /* last status line, shown in the overlay */
static np_info g_info;            /* from the launcher service */
static DWORD g_last_link_tick;

static void status(const char *fmt, ...) {
    np_status s = { PK_STATUS };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.text, sizeof s.text, fmt, ap);
    va_end(ap);
    logf_("status: %s", s.text);
    snprintf(g_status_text, sizeof g_status_text, "%s", s.text);
    send_raw(&s, 1 + (int)strlen(s.text) + 1);
}

/* ---- RNG (same LCG as MSVC CRT, but with a seed we own) ---- */

static int __cdecl my_rand(void) {
    g_rand_calls++;
    g_seed = g_seed * 0x343FDu + 0x269EC3u;
    return (int)((g_seed >> 16) & 0x7FFF);
}

static void __cdecl my_srand(unsigned s) {
    if (g_state == S_IDLE) g_seed = s;
}

/* ---- fixed timestep while a session runs ---- */

static void __fastcall my_timer(float *t, void *edx, int cap) {
    if (g_state != S_RUNNING) { orig_timer(t, edx, cap); return; }
    /* pace to FIXED_HZ in real time so the game does not run too fast */
    LARGE_INTEGER now;
    double period = (double)g_qpf.QuadPart / FIXED_HZ;
    for (;;) {
        QueryPerformanceCounter(&now);
        double el = (double)(now.QuadPart - g_last_frame.QuadPart);
        if (el >= period) break;
        if (period - el > g_qpf.QuadPart / 1000.0 * 2) Sleep(1);
    }
    /* after a stall do not try to catch up */
    if ((double)(now.QuadPart - g_last_frame.QuadPart) > period * 3) g_last_frame = now;
    else g_last_frame.QuadPart += (LONGLONG)period;
    orig_timer(t, edx, cap);
    /* overwrite what FUN_0043efb0 derived from the real clock */
    float dt = (float)(1.0 / FIXED_HZ) * t[1];
    t[2] = t[3] = t[4] = t[5] = t[6] = dt;
    float avg = (float)((dt * 4.0f) * *(double *)ADDR_TIMER_K);
    t[9] = avg;
    t[8] = avg * t[0];
    t[7] = t[1] / t[8];
}

/* ---- session ---- */

static int is_kbd_byte(uint32_t i) { return i >= KBD_FIRST && i < KBD_PREV_END; }
static int is_prev_byte(uint32_t i) { return i >= KBD_PREV_FIRST && i < KBD_PREV_END; }
static uint32_t local_len(void) { return g_role == ROLE_HOST ? INPUT_OBJ_SIZE : KBD_SIZE; }
static uint32_t remote_len(void) { return g_role == ROLE_HOST ? KBD_SIZE : INPUT_OBJ_SIZE; }
static const uint8_t *local_base(void) { return g_role == ROLE_HOST ? g_host_base : g_zero; }
static const uint8_t *remote_base(void) { return g_role == ROLE_HOST ? g_zero : g_host_base; }

/* client: progress goes to coop_save.dat next to the DLL, the friend's own save.dat stays */
static void redirect_save(void) {
    if (g_save_redirected) return;
    uint8_t pat[5] = { 0x68 };
    uint32_t name = ADDR_SAVE_NAME;
    memcpy(pat + 1, &name, 4);
    for (uint32_t p = ADDR_SAVE_FN; p < ADDR_SAVE_FN + 0x480; p++) {
        if (memcmp((void *)p, pat, 5) != 0) continue;
        uint32_t ours = (uint32_t)g_coop_save;
        g_save_redirected = write_code(p + 1, &ours, 4);
        logf_("save redirected at %08x: %d -> %s", p, g_save_redirected, g_coop_save);
        return;
    }
    logf_("save.dat push not found, save NOT redirected");
}

static void apply_host_settings(void) {
    if (!g_saved_settings) {
        g_own_profile_idx = GAME[GAME_PROFILE_IDX];
        g_own_blood = GAME[GAME_BLOOD];
        g_own_mouse_sens = *(int32_t *)(GAME + GAME_MOUSE_SENS);
        g_saved_settings = 1;
    }
    redirect_save();
    memcpy(GAME + GAME_PROFILES, g_start.profiles, GAME_PROFILES_LEN);
    GAME[GAME_PROFILE_IDX] = g_start.profile_idx;
    GAME[GAME_BLOOD] = g_start.blood;
    *(int32_t *)(GAME + GAME_MOUSE_SENS) = g_start.mouse_sens;
}

/* client, on exit: the co-op profile (progress of the sessions, kept in memory) is added to
   the friend's own save.dat as "КООП <name>" - a free slot or the same name again; the old
   file stays as save.dat.coopbak. The friend can go on with it alone or as a host. */
#define PROFILE_NAME_LEN 24
static void export_coop_profile(void) {
    char dir[MAX_PATH], path[MAX_PATH], bak[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\save.dat", dir);
    snprintf(bak, sizeof bak, "%s\\save.dat.coopbak", dir);
    uint8_t save[GAME_PROFILES_LEN] = { 0 };
    FILE *f = fopen(path, "rb");
    if (f) { fread(save, 1, sizeof save, f); fclose(f); }
    const uint8_t *coop = GAME + GAME_PROFILES + g_start.profile_idx * 0x13C;
    if (!coop[0]) return;
    char name[PROFILE_NAME_LEN] = { 0 };
    snprintf(name, sizeof name, "\xCA\xCE\xCE\xCF %s", (const char *)coop); /* "КООП " in CP1251 */
    int slot = -1;
    for (int i = 0; i < 6 && slot < 0; i++)
        if (!strncmp((const char *)save + i * 0x13C, name, PROFILE_NAME_LEN)) slot = i;
    for (int i = 0; i < 6 && slot < 0; i++)
        if (!save[i * 0x13C]) slot = i;
    if (slot < 0) { logf_("coop profile not exported: no free profile slot"); return; }
    CopyFileA(path, bak, FALSE);
    memcpy(save + slot * 0x13C, coop, 0x13C);
    memcpy(save + slot * 0x13C, name, PROFILE_NAME_LEN);
    f = fopen(path, "wb");
    if (f) { fwrite(save, 1, sizeof save, f); fclose(f); }
    logf_("coop profile exported to slot %d of %s", slot, path);
}

/* client: config.ini is written on exit, keep the friend's own settings in it */
static void restore_own_settings(void) {
    if (!g_saved_settings) return;
    static int exported;
    if (!exported++) export_coop_profile();
    GAME[GAME_PROFILE_IDX] = g_own_profile_idx;
    GAME[GAME_BLOOD] = g_own_blood;
    *(int32_t *)(GAME + GAME_MOUSE_SENS) = g_own_mouse_sens;
}

static void begin_running(void) {
    memset(g_mask, 0, sizeof g_mask);
    for (uint32_t i = 0; i < INPUT_OBJ_SIZE; i++) g_mask[i] = (uint8_t)np_mask_get(g_start.mask, i);
    memcpy(g_host_base, g_start.base, INPUT_OBJ_SIZE);
    g_delay = g_start.delay;
    memset(g_local, 0, sizeof g_local);
    memset(g_remote, 0, sizeof g_remote);
    for (uint32_t f = 0; f < g_delay; f++) memcpy(g_local[f].blob, local_base(), local_len());
    g_have_local = g_delay;
    g_have_remote = g_peer_ack = g_frame = 0;
    memset(g_my_checks, 0, sizeof g_my_checks);
    memset(g_peer_checks, 0, sizeof g_peer_checks);
    g_desync_reported = 0;
    g_link_lost = 0;
    g_npending = 0;
    g_stalls = 0;
    g_seed = g_start.seed;
    g_rand_calls = 0;
    g_nmouse = 0;
    g_level_loads = g_resync_id = 0;
    g_resync_have = 0;
    if (g_role == ROLE_CLIENT) apply_host_settings();
    g_last_peer_tick = g_last_status_tick = GetTickCount();
    QueryPerformanceCounter(&g_last_frame);
    g_state = S_RUNNING;
    status("Сессия началась: задержка ввода %u кадров (%u мс)", g_delay, g_delay * 1000 / 60);
}

static void end_session(const char *why) {
    if (g_state == S_IDLE) return;
    g_state = S_IDLE;
    status("Сессия остановлена: %s (кадр %u)", why, g_frame);
}

static void send_stop(void) {
    np_stop s = { PK_STOP, g_session };
    for (int i = 0; i < 5; i++) send_raw(&s, sizeof s);
}

static void send_start_chunks(void) {
    uint8_t pkt[NP_MAX_PACKET];
    for (uint32_t i = 0; i < NP_START_CHUNKS; i++) {
        if (g_acked_chunks & (1u << i)) continue;
        np_start_hdr *h = (np_start_hdr *)pkt;
        uint32_t off = i * NP_CHUNK;
        uint32_t len = sizeof(np_start) - off < NP_CHUNK ? sizeof(np_start) - off : NP_CHUNK;
        h->type = PK_START; h->session = g_session; h->idx = (uint16_t)i;
        h->total = (uint16_t)NP_START_CHUNKS; h->len = (uint16_t)len;
        memcpy(pkt + sizeof *h, (uint8_t *)&g_start + off, len);
        send_raw(pkt, sizeof *h + len);
    }
    g_last_chunk_tick = GetTickCount();
}

/* host, F8 in the main menu */
static void start_host(void) {
    uint8_t *obj = (uint8_t *)g_input_obj;
    if (!obj) { status("Нет объекта ввода, попробуйте ещё раз"); return; }
    memset(&g_start, 0, sizeof g_start);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_session = (uint32_t)now.QuadPart | 1;
    g_start.seed = (uint32_t)(now.QuadPart * 2654435761u);
    uint32_t d = (uint32_t)((g_rtt / 2 + 10) * 60 / 1000) + 1;
    g_start.delay = (uint8_t)(d < MIN_DELAY ? MIN_DELAY : d > MAX_DELAY ? MAX_DELAY : d);
    /* data bytes = everything except dwords that point into committed memory */
    for (uint32_t off = 0; off + 4 <= INPUT_OBJ_SIZE; off += 4) {
        uint32_t v = *(uint32_t *)(obj + off);
        MEMORY_BASIC_INFORMATION mbi;
        int ptr = v >= 0x400000 && VirtualQuery((void *)v, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT;
        for (uint32_t k = 0; k < 4; k++)
            if (!ptr || is_kbd_byte(off + k)) np_mask_set(g_start.mask, off + k);
    }
    for (uint32_t i = 0; i < INPUT_OBJ_SIZE; i++)
        g_start.base[i] = np_mask_get(g_start.mask, i) && !is_kbd_byte(i) ? obj[i] : 0;
    g_start.base[KBD_VALID] = 1;
    memcpy(g_start.profiles, GAME + GAME_PROFILES, GAME_PROFILES_LEN);
    g_start.profile_idx = GAME[GAME_PROFILE_IDX];
    g_start.blood = GAME[GAME_BLOOD];
    g_start.mouse_sens = *(int32_t *)(GAME + GAME_MOUSE_SENS);
    g_acked_chunks = 0;
    g_state = S_STARTING;
    status("Отправляю другу начальное состояние (%u частей)...", (unsigned)NP_START_CHUNKS);
    send_start_chunks();
}

static void compare_checks(uint32_t frame) {
    check_slot *a = &g_my_checks[frame % NP_RING], *b = &g_peer_checks[frame % NP_RING];
    if (g_desync_reported || !a->valid || !b->valid || a->frame != frame || b->frame != frame) return;
    if (a->seed == b->seed && a->calls == b->calls) return;
    g_desync_reported = 1;
    logf_("DESYNC frame %u: mine seed=%08x calls=%u, peer seed=%08x calls=%u",
          frame, a->seed, a->calls, b->seed, b->calls);
    status("РАССИНХРОН на кадре %u: игры разошлись. Остановите сессию (F8 у хоста) и начните заново из главного меню", frame);
}

static void on_peer_check(const np_check *c, void *ctx) {
    (void)ctx;
    check_slot *s = &g_peer_checks[c->frame % NP_RING];
    s->frame = c->frame; s->seed = c->seed; s->calls = c->calls; s->valid = 1;
    compare_checks(c->frame);
}

static void send_input(void) {
    uint8_t pkt[NP_MAX_PACKET];
    np_check checks[4];
    int nc = 0;
    for (uint32_t k = 1; k <= 4 && k <= g_frame; k++) {
        check_slot *s = &g_my_checks[(g_frame - k) % NP_RING];
        if (!s->valid) continue;
        checks[nc].frame = s->frame; checks[nc].seed = s->seed; checks[nc].calls = s->calls;
        nc++;
    }
    uint32_t first = g_peer_ack;
    if (g_have_local > NP_RING && first < g_have_local - NP_RING + 1) first = g_have_local - NP_RING + 1;
    int n = np_build_input(pkt, g_session, g_have_remote, g_local, local_base(), local_len(),
                           first, g_have_local, checks, nc);
    if (n > 0) send_raw(pkt, n);
    g_last_send_tick = GetTickCount();
}

/* ---- resync at level load (np_resync) ---- */

static np_resync g_resync;              /* host: what it sends; friend: what arrived */
static DWORD g_last_resync_tick;
static int g_resync_sends;
#define RESYNC_ALL ((NP_RESYNC_CHUNKS >= 64) ? ~0ull : ((1ull << NP_RESYNC_CHUNKS) - 1))

static int pointer_like(uint32_t v) {
    MEMORY_BASIC_INFORMATION mbi;
    return v >= 0x10000 && VirtualQuery((void *)v, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT;
}

static void send_resync_chunks(void) {
    uint8_t pkt[NP_MAX_PACKET];
    for (uint32_t i = 0; i < NP_RESYNC_CHUNKS; i++) {
        if (g_resync_have & (1ull << i)) continue;
        np_resync_hdr *h = (np_resync_hdr *)pkt;
        uint32_t off = i * NP_CHUNK;
        uint32_t len = sizeof(np_resync) - off < NP_CHUNK ? sizeof(np_resync) - off : NP_CHUNK;
        h->type = PK_RESYNC; h->session = g_session; h->id = g_resync_id; h->idx = (uint16_t)i; h->len = (uint16_t)len;
        memcpy(pkt + sizeof *h, (uint8_t *)&g_resync + off, len);
        send_raw(pkt, sizeof *h + len);
    }
    g_last_resync_tick = GetTickCount();
    g_resync_sends++;
}

/* host, right after the level was loaded */
static void resync_send(uint32_t id) {
    uint8_t *game = GAME;
    g_resync.seed = g_seed;
    memcpy(g_resync.players, game + GAME_PLAYERS, RESYNC_PLAYERS);
    memset(g_resync.pointers, 0, sizeof g_resync.pointers);
    for (uint32_t d = 0; d < RESYNC_PLAYERS / 4; d++)
        if (pointer_like(((uint32_t *)g_resync.players)[d])) g_resync.pointers[d >> 3] |= (uint8_t)(1u << (d & 7));
    memcpy(g_resync.profiles, game + GAME_PROFILES, GAME_PROFILES_LEN);
    g_resync_id = id;
    g_resync_have = 0;
    g_resync_sends = 0;
    send_resync_chunks();
}

/* friend: take the host's players and profiles, keeping our own pointers */
static void resync_apply(void) {
    uint8_t *game = GAME;
    uint32_t *dst = (uint32_t *)(game + GAME_PLAYERS);
    const uint32_t *src = (const uint32_t *)g_resync.players;
    int changed = 0;
    for (uint32_t d = 0; d < RESYNC_PLAYERS / 4; d++) {
        if ((g_resync.pointers[d >> 3] >> (d & 7)) & 1 || pointer_like(dst[d])) continue;
        if (dst[d] != src[d]) { dst[d] = src[d]; changed++; }
    }
    memcpy(game + GAME_PROFILES, g_resync.profiles, GAME_PROFILES_LEN);
    g_seed = g_resync.seed;
    logf_("resync %u applied, %d values differed", g_resync_id, changed);
    if (g_desync_reported) status("Игры снова синхронизированы (новый уровень)");
    g_desync_reported = 0;
}

static void on_packet(const uint8_t *p, int n, const struct sockaddr_in *from) {
    switch (p[0]) {
    case PK_INFO:
        if (n >= (int)sizeof(np_info)) {
            memcpy(&g_info, p, sizeof g_info);
            g_info.code[sizeof g_info.code - 1] = g_info.note[sizeof g_info.note - 1] = 0;
        }
        break;
    case PK_LINK: {
        if (n < (int)sizeof(np_link)) return;
        const np_link *l = (const np_link *)p;
        if (!g_link_ok) logf_("launcher linked, role=%u", l->role);
        g_last_link_tick = GetTickCount();
        g_launcher = *from;
        g_link_ok = 1;
        if (g_state == S_IDLE) g_role = l->role;
        if (l->connected && !g_peer_connected) logf_("peer connected");
        g_peer_connected = l->connected;
        g_rtt = l->rtt_ms;
        break;
    }
    case PK_START: {
        if (g_role != ROLE_CLIENT || n < (int)sizeof(np_start_hdr)) return;
        const np_start_hdr *h = (const np_start_hdr *)p;
        if (h->total != NP_START_CHUNKS || h->idx >= NP_START_CHUNKS) return;
        uint32_t off = h->idx * NP_CHUNK;
        if (h->len > NP_CHUNK || off + h->len > sizeof(np_start) || n < (int)(sizeof *h + h->len)) return;
        int running_this = g_state == S_RUNNING && g_session == h->session;
        if (!running_this) {
            if (h->session != g_rx_session) { g_rx_session = h->session; g_rx_chunks = 0; }
            memcpy((uint8_t *)&g_start + off, p + sizeof *h, h->len);
            g_rx_chunks |= 1u << h->idx;
        }
        np_start_ack a = { PK_START_ACK, h->session, running_this ? (1u << NP_START_CHUNKS) - 1 : g_rx_chunks };
        send_raw(&a, sizeof a);
        if (!running_this && g_rx_chunks == (1u << NP_START_CHUNKS) - 1) {
            g_session = h->session;
            begin_running();
        }
        break;
    }
    case PK_START_ACK: {
        if (g_role != ROLE_HOST || n < (int)sizeof(np_start_ack)) return;
        const np_start_ack *a = (const np_start_ack *)p;
        if (g_state != S_STARTING || a->session != g_session) return;
        g_acked_chunks |= a->have;
        if (g_acked_chunks == (1u << NP_START_CHUNKS) - 1) begin_running();
        break;
    }
    case PK_INPUT: {
        if (g_state != S_RUNNING || n < (int)sizeof(np_input_hdr)) return;
        const np_input_hdr *h = (const np_input_hdr *)p;
        if (h->session != g_session) return;
        if (!np_parse_input(p, n, g_remote, remote_base(), remote_len(), &g_have_remote, on_peer_check, NULL)) {
            logf_("bad INPUT packet (%d bytes)", n);
            return;
        }
        if (h->ack > g_peer_ack && h->ack <= g_have_local) g_peer_ack = h->ack;
        g_last_peer_tick = GetTickCount();
        if (g_link_lost) {
            g_link_lost = 0;
            status("Связь с другом восстановлена");
        }
        break;
    }
    case PK_RESYNC: {
        if (g_role != ROLE_CLIENT || g_state != S_RUNNING || n < (int)sizeof(np_resync_hdr)) return;
        const np_resync_hdr *h = (const np_resync_hdr *)p;
        uint32_t off = h->idx * NP_CHUNK;
        if (h->session != g_session || h->idx >= NP_RESYNC_CHUNKS || h->len > NP_CHUNK ||
            off + h->len > sizeof(np_resync) || n < (int)(sizeof *h + h->len) || h->id < g_resync_id) return;
        if (h->id > g_resync_id) { g_resync_id = h->id; g_resync_have = 0; }
        memcpy((uint8_t *)&g_resync + off, p + sizeof *h, h->len);
        g_resync_have |= 1ull << h->idx;
        np_resync_ack a = { PK_RESYNC_ACK, g_session, h->id, g_resync_have };
        send_raw(&a, sizeof a);
        break;
    }
    case PK_RESYNC_ACK: {
        const np_resync_ack *a = (const np_resync_ack *)p;
        if (g_role == ROLE_HOST && n >= (int)sizeof *a && a->session == g_session && a->id == g_resync_id)
            g_resync_have |= a->have;
        break;
    }
    case PK_STOP: {
        const np_stop *s = (const np_stop *)p;
        if (n >= (int)sizeof *s && s->session == g_session) end_session("остановлено другой стороной");
        break;
    }
    }
}

static void net_poll(void);

/* FUN_004084f0 (fastcall game): loads a level. In a session both games are inside the same
   frame here: the RNG is reseeded alike, the host sends its players, the friend waits for
   them and takes them before the frame goes on. */
#define ADDR_LEVEL_LOAD  0x004084F0u
typedef int(__fastcall *level_fn)(void *game);
static level_fn orig_level_load = (level_fn)ADDR_LEVEL_LOAD;

static int __fastcall my_level_load(void *game) {
    int r = orig_level_load(game);
    if (g_state != S_RUNNING) return r;
    uint32_t id = ++g_level_loads;
    g_seed = g_session * 2654435761u ^ id * 0x9E3779B9u;
    if (g_role == ROLE_HOST) { resync_send(id); return r; }
    DWORD t0 = GetTickCount();
    while (!(g_resync_id == id && g_resync_have == RESYNC_ALL)) {
        if (g_state != S_RUNNING || GetTickCount() - t0 > 10000) {
            logf_("resync %u: no data from the host", id);
            return r;
        }
        net_poll();
        Sleep(1);
    }
    resync_apply();
    return r;
}

static void net_poll(void) {
    if (g_sock == INVALID_SOCKET) return;
    uint8_t buf[2048];
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(g_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || from.sin_port != htons(NP_PORT_LAUNCHER)) continue;
        on_packet(buf, n, &from);
    }
}

/* ---- input ---- */


/* The mouse object (input+0x210) accumulates: cursor = old cursor + delta, clamped, and the
   game moves the cursor itself (spawn). So the mouse is not copied as state: the raw
   DirectInput events (buffered GetDeviceData, FUN_0043e7e0) travel instead, and both games
   run them through their own poll. The keyboard and joysticks are plain device state. */
typedef HRESULT(__stdcall *getdata_fn)(void *dev, DWORD cb, void *items, DWORD *inout, DWORD flags);
typedef struct { DWORD ofs, data, time, seq, app; } di_item;
static void *g_mouse_vtbl[32];
static getdata_fn real_getdata;
static void *g_mouse_dev;
static int g_in_poll, g_mouse_drained, g_mouse_serve;
static np_msg g_mouse_pending[NP_MAX_MSGS];

/* device state copied from the host: keyboard keys and the two joystick blocks. The mouse
   goes as raw events; the rest of the object (0x620..) is the game's own derived state. */
static int is_mouse_byte(uint32_t i) { return !((i >= KBD_FIRST && i < KBD_PREV_FIRST) || (i >= 0x2B0 && i < 0x620)); }

static void collect_mouse(const di_item *it) {
    /* movement summed per axis (one item each), button presses kept in order */
    if (it->ofs <= 8) {
        for (int k = 0; k < g_nmouse; k++)
            if (g_mouse_pending[k].wparam == it->ofs) { g_mouse_pending[k].lparam += it->data; return; }
    }
    if (g_nmouse < NP_MAX_MSGS - 8) {
        np_msg m = { MSG_MOUSE_ITEM, it->ofs, it->data };
        g_mouse_pending[g_nmouse++] = m;
    }
}

/* Menus (and the pause menu) take input from both players: the friend picks a hero and
   types a name with the same cursor. In a level only the host's mouse is used. Decided from
   the game state at the start of the frame, so both games decide alike. */
static int g_share_ui;
static int g_pause_by;          /* role whose Esc paused the game last (overlay) */
static np_msg g_serve[2 * NP_MAX_MSGS];
static int g_nserve;

static int shared_ui(const uint8_t *game) {
    return *(const int *)(game + 0xB68) != 2 || *(const int *)(game + 0xB80) == 3;
}

static void build_mouse_serve(void) {
    const np_frame *h = g_role == ROLE_HOST ? &g_local[g_frame % NP_RING] : &g_remote[g_frame % NP_RING];
    const np_frame *c = g_role == ROLE_HOST ? &g_remote[g_frame % NP_RING] : &g_local[g_frame % NP_RING];
    g_nserve = 0;
    for (int i = 0; i < h->nmsg; i++) if (h->msgs[i].msg == MSG_MOUSE_ITEM) g_serve[g_nserve++] = h->msgs[i];
    if (g_share_ui)
        for (int i = 0; i < c->nmsg; i++) if (c->msgs[i].msg == MSG_MOUSE_ITEM) g_serve[g_nserve++] = c->msgs[i];
}

static HRESULT __stdcall my_getdata(void *dev, DWORD cb, void *items, DWORD *inout, DWORD flags) {
    if (g_state != S_RUNNING || !g_in_poll || cb < sizeof(di_item)) return real_getdata(dev, cb, items, inout, flags);
    if (!g_mouse_drained) {
        g_mouse_drained = 1;
        for (int guard = 0; guard < 512; guard++) {
            di_item it;
            DWORD n = 1;
            if (real_getdata(dev, sizeof it, &it, &n, 0) < 0 || n == 0) break;
            collect_mouse(&it); /* the friend's mouse is used in menus (g_share_ui) */
        }
    }
    if (g_mouse_serve < g_nserve && inout && *inout >= 1 && items) {
        di_item *it = (di_item *)items;
        memset(items, 0, cb);
        it->ofs = g_serve[g_mouse_serve].wparam;
        it->data = g_serve[g_mouse_serve].lparam;
        it->seq = (DWORD)g_frame;
        *inout = 1;
        g_mouse_serve++;
    } else if (inout) *inout = 0;
    return 0;
}

static void hook_mouse(uint8_t *obj) {
    void *dev = *(void **)(obj + 0x210);
    if (!dev || dev == g_mouse_dev) return;
    void **vt = *(void ***)dev;
    if (vt == g_mouse_vtbl) return;
    memcpy(g_mouse_vtbl, vt, sizeof g_mouse_vtbl);
    real_getdata = (getdata_fn)vt[10];
    g_mouse_vtbl[10] = (void *)my_getdata;
    *(void ***)dev = g_mouse_vtbl;
    g_mouse_dev = dev;
    logf_("mouse device %p hooked (immediate mode flag %u)", dev, obj[0]);
}

/* ---- AI for player 2 in the bot test ----
   Reads the level from memory and presses player 2's keys (control scheme 1):
   W/A/S/D move up/left/down/right on screen, H/K turn the aim, J fires.
   Players: game+0x1738 + i*0x22DC (+0 active, +0x190/+0x194 position, +0x1B0 health,
   +0x22B0 aim angle). Enemies: count game+0xFB54, pointers game+0x126C0 (+0x2C/+0x30
   position, +0x502 active). Projectiles: count game+0xFB50, pointers game+0xFFB0 (+4/+8
   position, +0x54/+0x58 velocity per frame, +0x18 type, +0x20 owner, -1 = enemies).
   Only the host's game runs it, so it does not need to be deterministic. */
#include <math.h>

static int g_ai;                 /* the bot test: player 2 is played by this AI */

#define P_SIZE      0x22DCu
#define P_ACTIVE    0x0u
#define P_X         0x190u
#define P_Y         0x194u
#define P_HEALTH    0x1B0u
#define P_ANGLE     0x22B0u
#define DIK_W 0x11
#define DIK_A 0x1E
#define DIK_S 0x1F
#define DIK_D 0x20
#define DIK_H 0x23
#define DIK_J 0x24
#define DIK_K 0x25
#define DIK_U 0x16

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if ((uint32_t)p < 0x10000 || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint8_t *)p + n <= (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
}

static float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static void ai_keys(uint8_t *kb) {
    static float last_x, last_y, unstuck_dx, unstuck_dy;
    static int still_frames, unstuck_left, frame;
    uint8_t *game = GAME;
    memset(kb, 0, KBD_SIZE);
    frame++;
    if (*(int *)(game + 0xB68) != 2 || *(int *)(game + 0xB80) == 3) return; /* menus, pause */
    uint8_t *me = game + GAME_PLAYERS + P_SIZE, *host = game + GAME_PLAYERS;
    int alive = me[P_ACTIVE] && *(float *)(me + P_HEALTH) > 0;
    float x = *(float *)(me + P_X), y = *(float *)(me + P_Y), ang = *(float *)(me + P_ANGLE);

    if (!alive) return;

    /* target: the nearest living enemy */
    int n = *(int *)(game + 0xFB54);
    float best = 1e30f, tx = 0, ty = 0, mx = 0, my = 0; /* mx,my: push away from danger */
    for (int i = 0; i < n && i < 512; i++) {
        uint8_t *m = *(uint8_t **)(game + 0x126C0 + i * 4);
        if (!readable(m, 0x510) || !m[0x502]) continue;
        float dx = *(float *)(m + 0x2C) - x, dy = *(float *)(m + 0x30) - y, d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; tx = dx; ty = dy; }
        if (d2 < 140.0f * 140.0f && d2 > 1) { float d = sqrtf(d2); mx -= dx / d * (140 - d) / 140; my -= dy / d * (140 - d) / 140; }
    }

    /* danger: enemy shots that will pass close, and lying mines and bombs */
    int np = *(int *)(game + 0xFB50);
    for (int i = 0; i < np && i < 1024; i++) {
        uint8_t *p = *(uint8_t **)(game + 0xFFB0 + i * 4);
        if (!readable(p, 0xB8) || *(int *)(p + 0x20) != -1) continue;
        float rx = *(float *)(p + 4) - x, ry = *(float *)(p + 8) - y;
        float vx = *(float *)(p + 0x54), vy = *(float *)(p + 0x58), v2 = vx * vx + vy * vy;
        float r2 = rx * rx + ry * ry;
        if (r2 > 400.0f * 400.0f) continue;
        if (v2 < 0.01f) {                       /* lying still: keep away */
            if (r2 < 110.0f * 110.0f && r2 > 1) { float d = sqrtf(r2); mx -= rx / d * 1.5f; my -= ry / d * 1.5f; }
            continue;
        }
        /* anything hostile right next to us (flame, slow clouds): step out of it */
        if (r2 < 90.0f * 90.0f && r2 > 1) { float d = sqrtf(r2); mx -= rx / d * (90 - d) / 45; my -= ry / d * (90 - d) / 45; }
        float t = -(rx * vx + ry * vy) / v2;    /* frames until the closest approach */
        if (t < 0 || t > 60) continue;
        float cx = rx + vx * t, cy = ry + vy * t, miss = sqrtf(cx * cx + cy * cy);
        if (miss > 70) continue;
        /* step sideways, away from the line it flies on */
        float sx = -vy, sy = vx, sl = sqrtf(v2);
        if (sx * cx + sy * cy > 0) { sx = -sx; sy = -sy; }
        float w = (70 - miss) / 70 * (1.5f - t / 60);
        mx += sx / sl * w * 2; my += sy / sl * w * 2;
    }

    /* stay near the host */
    float hx = *(float *)(host + P_X) - x, hy = *(float *)(host + P_Y) - y, hd = sqrtf(hx * hx + hy * hy);
    if (host[P_ACTIVE] && hd > 300) { mx += hx / hd * 0.6f; my += hy / hd * 0.6f; }

    /* unstuck: pushing but not moving -> slide sideways for a while */
    float moved = fabsf(x - last_x) + fabsf(y - last_y);
    last_x = x; last_y = y;
    int pushing = fabsf(mx) > 0.25f || fabsf(my) > 0.25f;
    still_frames = pushing && moved < 0.2f ? still_frames + 1 : 0;
    if (still_frames > 20 && !unstuck_left) {
        unstuck_left = 40;
        unstuck_dx = (frame & 1) ? -my : my;
        unstuck_dy = (frame & 1) ? mx : -mx;
    }
    if (unstuck_left) { unstuck_left--; mx = unstuck_dx; my = unstuck_dy; }

    if (mx < -0.25f) kb[DIK_A] = 0x80;
    if (mx > 0.25f) kb[DIK_D] = 0x80;
    if (my < -0.25f) kb[DIK_W] = 0x80;
    if (my > 0.25f) kb[DIK_S] = 0x80;

    /* aim: angle 0 = up, clockwise (projectiles: x += sin a, y -= cos a) */
    if (best < 900.0f * 900.0f) {
        float want = atan2f(tx, -ty), diff = wrap_pi(want - ang);
        if (diff > 0.04f) kb[DIK_K] = 0x80;
        else if (diff < -0.04f) kb[DIK_H] = 0x80;
        if (fabsf(diff) < 0.2f) {
            /* alternative fire (U) on a close target or a crowd, in bursts; else the main gun */
            static int alt_cool;
            int crowd = 0;
            for (int i = 0; i < n && i < 512; i++) {
                uint8_t *m = *(uint8_t **)(game + 0x126C0 + i * 4);
                if (!readable(m, 0x510) || !m[0x502]) continue;
                float dx = *(float *)(m + 0x2C) - x, dy = *(float *)(m + 0x30) - y;
                crowd += dx * dx + dy * dy < 320.0f * 320.0f;
            }
            if (alt_cool > 0) alt_cool--;
            if ((crowd >= 2 || best < 250.0f * 250.0f) && alt_cool == 0) { kb[DIK_U] = 0x80; if (frame % 30 == 29) alt_cool = 90; }
            else kb[DIK_J] = 0x80;
        }
        /* weapons 1/2/3 belong to player 2 in co-op: switch now and then while fighting */
        static int weapon_timer, weapon;
        if (++weapon_timer > 480) {
            weapon_timer = 0;
            weapon = (weapon + 1) % 3;
            kb[2 + weapon] = 0x80; /* DIK_1..DIK_3 */
        }
    }
}

/* ---- gamepad for player 2 ----
   An XInput pad presses player 2's keys (control scheme 1): left stick / d-pad = W A S D,
   right stick = turn (H/K) towards where it points, LB/RB = turn, RT or A = fire (J),
   LT or B = alternative fire (U), X/Y/back... = weapons 1/2/3, Start = Esc. */
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } xpad;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void *);

static int g_pad_ok;                    /* a pad was found; shown in the co-op screen */
static DWORD g_pad_next_load;
static void pad_found(DWORD index) {
    if (g_pad_ok) return;
    g_pad_ok = 1;
    logf_("gamepad connected on slot %lu", (unsigned long)index);
}

static int pad_read(xpad *p) {
    static XInputGetState_t get;
    static DWORD next_try, pad_index;
    if (!get && GetTickCount() >= g_pad_next_load) {
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput1_3.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
        g_pad_next_load = GetTickCount() + 5000;
    }
    if (!get) return 0;
    if (GetTickCount() < next_try) return 0;
    struct { DWORD packet; xpad pad; } st;
    if (get(pad_index, &st) == 0) { *p = st.pad; pad_found(pad_index); return 1; }
    /* not connected: look at the other slots, at most once a second */
    for (DWORD i = 0; i < 4; i++)
        if (get(i, &st) == 0) { pad_index = i; *p = st.pad; pad_found(i); return 1; }
    if (g_pad_ok) { g_pad_ok = 0; logf_("gamepad disconnected"); }
    next_try = GetTickCount() + 1000;
    return 0;
}

static xpad g_pad_state;
static int g_pad_have;

static void pad_poll(void) {
    xpad p;
    g_pad_have = pad_read(&p);
    if (g_pad_have) g_pad_state = p;
}

static void pad_keys(uint8_t *kb) {
    if (!g_pad_have) return;
    xpad p = g_pad_state;
    const int dz = 12000;
    if (p.lx < -dz || (p.buttons & 0x0004)) kb[DIK_A] = 0x80;
    if (p.lx > dz || (p.buttons & 0x0008)) kb[DIK_D] = 0x80;
    if (p.ly > dz || (p.buttons & 0x0001)) kb[DIK_W] = 0x80;
    if (p.ly < -dz || (p.buttons & 0x0002)) kb[DIK_S] = 0x80;
    if (p.buttons & 0x0100) kb[DIK_H] = 0x80;               /* LB */
    if (p.buttons & 0x0200) kb[DIK_K] = 0x80;               /* RB */
    if (p.rt > 60 || (p.buttons & 0x1000)) kb[DIK_J] = 0x80; /* RT, A */
    if (p.lt > 60 || (p.buttons & 0x2000)) kb[DIK_U] = 0x80; /* LT, B */
    if (p.buttons & 0x4000) kb[2] = 0x80;                   /* X: weapon 1 */
    if (p.buttons & 0x8000) kb[3] = 0x80;                   /* Y: weapon 2 */
    if (p.buttons & 0x0020) kb[4] = 0x80;                   /* Back: weapon 3 */
    if (p.buttons & 0x0010) kb[1] = 0x80;                   /* Start: Esc */
    /* right stick: turn player 2 towards it (angle 0 = up, clockwise) */
    float rx = p.rx, ry = p.ry;
    uint8_t *game = GAME;
    if (rx * rx + ry * ry > 16000.0f * 16000.0f && *(int *)(game + 0xB68) == 2) {
        uint8_t *me = game + GAME_PLAYERS + P_SIZE;
        float diff = wrap_pi(atan2f(rx, ry) - *(float *)(me + P_ANGLE));
        if (diff > 0.06f) kb[DIK_K] = 0x80;
        else if (diff < -0.06f) kb[DIK_H] = 0x80;
    }
}

static void __fastcall my_poll(void *input) {
    g_input_obj = input;
    uint8_t *obj = (uint8_t *)input;
    hook_mouse(obj);
    g_in_poll = 1;
    g_mouse_drained = 0;
    g_mouse_serve = 0;
    if (g_state == S_RUNNING) build_mouse_serve();
    orig_poll(input);
    g_in_poll = 0;
    pad_poll();
    /* the pad plays player 2: alone at one computer, or as the friend; the host is player 1 */
    if (obj[KBD_VALID] && (g_state != S_RUNNING || g_role != ROLE_HOST)) pad_keys(obj + KBD_FIRST);
    if (g_state != S_RUNNING) return;
    int kbd_ok = obj[KBD_VALID] != 0; /* when GetDeviceState fails the old merged keys stay */

    /* our own input becomes the input of frame g_frame + delay */
    if (g_frame + g_delay == g_have_local) {
        np_frame *lf = &g_local[g_have_local % NP_RING];
        if (g_role == ROLE_HOST) {
            for (uint32_t i = 0; i < INPUT_OBJ_SIZE; i++)
                lf->blob[i] = g_mask[i] && !is_prev_byte(i) && !is_mouse_byte(i) && (kbd_ok || !is_kbd_byte(i)) ? obj[i] : 0;
        } else if (kbd_ok) memcpy(lf->blob, obj + KBD_FIRST, KBD_SIZE);
        else memset(lf->blob, 0, KBD_SIZE);
        /* mouse events and window messages of both sides travel; the friend's count in menus */
        memcpy(lf->msgs, g_mouse_pending, g_nmouse * sizeof(np_msg));
        int n = g_npending < NP_MAX_MSGS - g_nmouse ? g_npending : NP_MAX_MSGS - g_nmouse;
        memcpy(lf->msgs + g_nmouse, g_pending, n * sizeof(np_msg));
        lf->nmsg = (uint8_t)(n + g_nmouse);
        g_nmouse = 0;
        memmove(g_pending, g_pending + n, (g_npending - n) * sizeof(np_msg));
        g_npending -= n;
        g_have_local++;
    }

    /* the bot test: the AI plays player 2 instead of the bot's scripted keys */
    if (g_ai && g_role == ROLE_HOST) ai_keys(g_remote[g_frame % NP_RING].blob);

    /* merged input of this frame: host input, keyboards OR'ed */
    const np_frame *h = g_role == ROLE_HOST ? &g_local[g_frame % NP_RING] : &g_remote[g_frame % NP_RING];
    const np_frame *c = g_role == ROLE_HOST ? &g_remote[g_frame % NP_RING] : &g_local[g_frame % NP_RING];
    for (uint32_t i = 0; i < INPUT_OBJ_SIZE; i++)
        if (g_mask[i] && !is_prev_byte(i) && !is_mouse_byte(i)) obj[i] = h->blob[i];
    for (uint32_t k = 0; k < KBD_SIZE; k++) obj[KBD_FIRST + k] = h->blob[KBD_FIRST + k] | c->blob[k];
    obj[KBD_VALID] = 1;
}

/* ---- window messages ---- */

static void queue_message(uint32_t msg, uint32_t wparam, uint32_t lparam) {
    /* coalesce mouse moves: only the latest position matters */
    if (msg == WM_MOUSEMOVE && g_npending > 0 && g_pending[g_npending - 1].msg == WM_MOUSEMOVE &&
        g_pending[g_npending - 1].wparam == wparam) {
        g_pending[g_npending - 1].lparam = lparam;
        return;
    }
    if (g_npending == (int)(sizeof g_pending / sizeof g_pending[0])) return;
    np_msg m = { (uint16_t)msg, wparam, lparam };
    g_pending[g_npending++] = m;
}

static int screen_message(uint32_t msg, uint32_t wparam);

static int __fastcall my_msgproc(void *game, void *edx, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    if (msg == WM_DESTROY) restore_own_settings();
    if (screen_message(msg, wparam)) return 0;
    if (g_state == S_RUNNING && !g_in_dispatch && msg != WM_DESTROY && msg != WM_CLOSE) {
        /* both games see the host's messages at the same frame; the client's are dropped */
        /* the friend's focus changes must not reach the game: it pauses on deactivation */
        if (msg < MSG_MOUSE_ITEM && (g_role == ROLE_HOST || (msg != WM_ACTIVATE && msg != WM_ACTIVATEAPP &&
                                                            msg != WM_KILLFOCUS && msg != WM_SETFOCUS)))
            queue_message(msg, wparam, lparam);
        return 0;
    }
    return orig_msgproc(game, edx, msg, wparam, lparam);
}

/* ---- frame ---- */

static int key_edge(int vk) {
    static SHORT prev[256];
    SHORT s = GetAsyncKeyState(vk);
    int down = (s & 0x8000) && !(prev[vk] & 0x8000);
    prev[vk] = s;
    return down;
}

/* ---- co-op screen ----
   Opened by the main menu button MULTIPLAYER (its old LAN deathmatch screen is replaced,
   see hook_multiplayer_button) or F7. Drawn by a click-through topmost window over the
   game; the buttons are hit with the game's own cursor (WM_LBUTTONDOWN in my_msgproc).
   The network side is rip3coop.exe, started hidden on demand (--service), driven with PK_CMD. */

static HWND g_overlay;
static volatile int g_screen_open;
static int g_mode;                       /* 0 = internet/STUN, 1 = Radmin VPN */
static uint8_t g_queued_cmd;
static char g_queued_text[64];

static DWORD g_wait_tick;   /* a create/join click is being prepared (STUN takes seconds) */
static int link_alive(void) { return g_link_ok && GetTickCount() - g_last_link_tick < 8000; }
static int preparing(void) { return g_wait_tick && g_info.state == INFO_IDLE && GetTickCount() - g_wait_tick < 20000; }

static void start_service(void) {
    static DWORD last_try;
    if (link_alive() || GetTickCount() - last_try < 3000) return;
    last_try = GetTickCount();
    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof cmd, "\"%s\\rip3coop.exe\" --service %lu", g_dir, GetCurrentProcessId());
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        logf_("service started");
    } else logf_("service start failed (%lu): %s", GetLastError(), cmd);
}

static void send_cmd(uint8_t op, const char *text) {
    if (!link_alive()) {
        g_queued_cmd = op;
        snprintf(g_queued_text, sizeof g_queued_text, "%s", text ? text : "");
        start_service();
        return;
    }
    np_cmd c = { PK_CMD, op };
    snprintf(c.text, sizeof c.text, "%s", text ? text : "");
    send_raw(&c, sizeof c);
}

static void paste_code(void) {
    char text[64] = "";
    if (OpenClipboard(NULL)) {
        HANDLE h = GetClipboardData(CF_TEXT);
        const char *s = h ? (const char *)GlobalLock(h) : NULL;
        if (s) { snprintf(text, sizeof text, "%s", s); GlobalUnlock(h); }
        CloseClipboard();
    }
    send_cmd(CMD_PEER_CODE, text);
}

static void copy_code(void) {
    if (!g_info.code[0] || !OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t n = strlen(g_info.code) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) { memcpy(GlobalLock(h), g_info.code, n); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
}

static void disconnect(void) {
    g_ai = 0;
    if (g_state != S_IDLE) { send_stop(); end_session("отключено"); }
    if (link_alive()) send_cmd(CMD_QUIT, NULL);
    g_queued_cmd = 0;
    g_wait_tick = 0;
    g_link_ok = g_peer_connected = 0;
    memset(&g_info, 0, sizeof g_info);
    g_status_text[0] = 0;
}

/* screen layout in game space: 1024x768, origin in the centre (the cursor's space) */
enum { B_HOST = 1, B_JOIN, B_BOT, B_PASTE, B_COPY, B_START, B_STOP, B_DISCONNECT, B_BACK, B_MODE };
typedef struct { int id; const char *label; } button;
#define SCR_W 560
#define BTN_W 380
#define BTN_H 40
#define BTN_GAP 10

static int screen_content(char lines[6][200], button *b, int *nb) {
    int n = 0;
    *nb = 0;
    const char *role = g_info.role == ROLE_HOST ? "хост (первый игрок)" : "второй игрок";
    int launching = g_queued_cmd || preparing() || (link_alive() && g_info.state == INFO_IDLE && !g_info.note[0]);
    if (launching) {
        snprintf(lines[n++], 200, g_mode ? "Подготовка...  Беру адрес Radmin VPN."
                                         : "Подготовка...  Узнаю внешний адрес.");
    } else if (!link_alive() || g_info.state == INFO_IDLE) {
        snprintf(lines[n++], 200, "Способ: %s", g_mode ? "Radmin VPN (26.x)" : "интернет, код через STUN");
        if (g_mode) snprintf(lines[n++], 200, "Разрешите UDP 27605 в брандмауэре Windows.");
        if (g_pad_ok) snprintf(lines[n++], 200, "Геймпад: подключён (игрок 2).");
        snprintf(lines[n++], 200, "Хост ведёт меню, друг - второй игрок.");
        b[(*nb)++] = (button){ B_MODE, g_mode ? "Переключить на интернет" : "Переключить на Radmin VPN" };
        b[(*nb)++] = (button){ B_HOST, "Создать игру" };
        b[(*nb)++] = (button){ B_JOIN, "Подключиться к другу" };
        b[(*nb)++] = (button){ B_BOT, "Тест: бот - второй игрок" };
    } else if (g_info.state == INFO_ERROR) {
        snprintf(lines[n++], 200, "%s", g_info.note[0] ? g_info.note : "Ошибка соединения.");
        b[(*nb)++] = (button){ B_DISCONNECT, "Попробовать снова" };
    } else {
        snprintf(lines[n++], 200, "Вы: %s   Способ: %s", role, g_mode ? "Radmin VPN" : "интернет");
        if (g_info.code[0] && g_info.state != INFO_CONNECTED) snprintf(lines[n++], 200, "Ваш код: %s", g_info.code);
        if (g_info.state == INFO_NEED_CODE) {
            snprintf(lines[n++], 200, "Отправьте код другу, скопируйте его код");
            snprintf(lines[n++], 200, "и нажмите «Вставить код друга».");
            b[(*nb)++] = (button){ B_PASTE, "Вставить код друга" };
            b[(*nb)++] = (button){ B_COPY, "Скопировать мой код" };
        } else if (g_info.state == INFO_CONNECTING) {
            snprintf(lines[n++], 200, "%s", g_info.note);
        } else {
            snprintf(lines[n++], 200, "Соединено, пинг %u мс", g_info.rtt_ms);
            if (g_info.role == ROLE_HOST) {
                if (g_state == S_IDLE) {
                    snprintf(lines[n++], 200, "Нажмите «Начать», затем «ДВА ИГРОКА».");
                    b[(*nb)++] = (button){ B_START, "Начать" };
                } else b[(*nb)++] = (button){ B_STOP, "Остановить сессию" };
            } else snprintf(lines[n++], 200, g_state == S_IDLE ? "Ждите: хост начнёт игру." : "Сессия идёт.");
        }
        if (g_status_text[0] && n < 6) snprintf(lines[n++], 200, "%s", g_status_text);
        b[(*nb)++] = (button){ B_DISCONNECT, "Отключиться" };
    }
    b[(*nb)++] = (button){ B_BACK, "Назад" };
    return n;
}

/* game-space rectangles of the panel and its buttons */
static int screen_height(int nlines, int nb) { return 70 + nlines * 30 + 20 + nb * (BTN_H + BTN_GAP) + 10; }
static RECT button_rect(int nlines, int nb, int i) {
    int top = -screen_height(nlines, nb) / 2 + 70 + nlines * 30 + 20 + i * (BTN_H + BTN_GAP);
    RECT r = { -BTN_W / 2, top, BTN_W / 2, top + BTN_H };
    return r;
}

static void screen_click(float cx, float cy) {
    char lines[6][200];
    button b[8];
    int nb, n = screen_content(lines, b, &nb);
    for (int i = 0; i < nb; i++) {
        RECT r = button_rect(n, nb, i);
        if (cx < r.left || cx >= r.right || cy < r.top || cy >= r.bottom) continue;
        switch (b[i].id) {
        case B_HOST: g_wait_tick = GetTickCount(); send_cmd(CMD_HOST, g_mode ? "radmin" : NULL); break;
        case B_JOIN: g_wait_tick = GetTickCount(); send_cmd(CMD_JOIN, g_mode ? "radmin" : NULL); break;
        case B_BOT: g_wait_tick = GetTickCount(); g_ai = 1; send_cmd(CMD_BOT_FRIEND, NULL); break;
        case B_PASTE: paste_code(); break;
        case B_COPY: copy_code(); break;
        case B_START: start_host(); g_screen_open = 0; break;
        case B_STOP: send_stop(); end_session("хост остановил"); break;
        case B_DISCONNECT: disconnect(); break;
        case B_BACK:
            if (g_info.state == INFO_ERROR) disconnect();
            g_screen_open = 0;
            break;
        case B_MODE: g_mode = !g_mode; break;
        }
        return;
    }
}

/* called by the patched MULTIPLAYER button check in FUN_0040f600 (registers saved) */
static void __cdecl open_coop_screen(void) { g_screen_open = 1; }

static int hook_multiplayer_button(void) {
    /* buttons: game+0x15418 is MULTIPLAYER (created at y=-20 in FUN_0040c210).
       0x41029C: mov ecx,[ebp+0x15418]; cmp [ecx+0x118],edi; jnz short 0x4102EE (not clicked).
       The cmp + jnz (8 bytes at 0x4102A2) become a jmp to the stub, which redoes the cmp. */
    static const uint8_t expect[] = { 0x8B, 0x8D, 0x18, 0x54, 0x01, 0x00, 0x39, 0xB9, 0x18, 0x01, 0x00, 0x00, 0x75, 0x44 };
    const uint32_t at = 0x0041029Cu, cmp = at + 6, not_clicked = 0x004102EEu;
    if (memcmp((void *)at, expect, sizeof expect) != 0) { logf_("MULTIPLAYER button code not found"); return 0; }
    uint8_t *stub = VirtualAlloc(NULL, 48, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    int k = 0;
    int32_t rel;
    static const uint8_t cmp_insn[] = { 0x39, 0xB9, 0x18, 0x01, 0x00, 0x00 }; /* cmp [ecx+0x118],edi */
    memcpy(stub, cmp_insn, sizeof cmp_insn);
    k = sizeof cmp_insn;
    stub[k++] = 0x0F; stub[k++] = 0x85;                 /* jnz not_clicked */
    rel = (int32_t)(not_clicked - ((uint32_t)stub + k + 4)); memcpy(stub + k, &rel, 4); k += 4;
    stub[k++] = 0x60; stub[k++] = 0x9C;                 /* pushad; pushfd */
    stub[k++] = 0xE8;                                   /* call open_coop_screen */
    rel = (int32_t)((uint32_t)open_coop_screen - ((uint32_t)stub + k + 4)); memcpy(stub + k, &rel, 4); k += 4;
    stub[k++] = 0x9D; stub[k++] = 0x61;                 /* popfd; popad */
    stub[k++] = 0xE9;                                   /* jmp not_clicked: the deathmatch screen is skipped */
    rel = (int32_t)(not_clicked - ((uint32_t)stub + k + 4)); memcpy(stub + k, &rel, 4); k += 4;
    uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    rel = (int32_t)((uint32_t)stub - (cmp + 5));
    memcpy(patch + 1, &rel, 4);
    return write_code(cmp, patch, sizeof patch);
}

static void screen_keys(int fg) {
    if (g_queued_cmd && link_alive()) { uint8_t op = g_queued_cmd; g_queued_cmd = 0; send_cmd(op, g_queued_text); }
    int f7 = key_edge(VK_F7), v = key_edge('V');
    if (!fg) return;
    if (f7) g_screen_open = !g_screen_open;
    if (g_screen_open && v && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && g_info.state == INFO_NEED_CODE) paste_code();
    /* the friend: the session started, back to the game */
    if (g_role == ROLE_CLIENT && g_state == S_RUNNING && g_screen_open) {
        static uint32_t seen_session;
        if (seen_session != g_session) { seen_session = g_session; g_screen_open = 0; }
    }
}

/* The mod settings live in the main menu, not in the co-op screen. */
static int main_menu_visible(void) {
    return g_state == S_IDLE && *(const int *)(GAME + 0xB68) == 1 && *(const int *)(GAME + 0xB74) == 0;
}
/* my_msgproc: while the screen is open the menu underneath gets no clicks or keys */
static int screen_message(uint32_t msg, uint32_t wparam) {
    if (!g_screen_open || g_in_dispatch) return 0;
    if (msg == WM_LBUTTONDOWN && g_input_obj) {
        uint8_t *in = (uint8_t *)g_input_obj;
        screen_click(*(float *)(in + 0x26C), *(float *)(in + 0x270));
        return 1;
    }
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) { g_screen_open = 0; return 1; }
    return msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_LBUTTONDBLCLK ||
           msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR;
}

static void text_w(HDC dc, int x, int y, const char *s, int center_w) {
    wchar_t w[200];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 200) - 1;
    if (n < 0) return;
    if (center_w) {
        SIZE sz;
        GetTextExtentPoint32W(dc, w, n, &sz);
        x += (center_w - sz.cx) / 2;
    }
    TextOutW(dc, x, y, w, n);
}

#define OV_KEY RGB(255, 0, 255)

static void paint_overlay(HDC dc, RECT *rc) {
    HBRUSH key = CreateSolidBrush(OV_KEY);
    FillRect(dc, rc, key);
    DeleteObject(key);
    SetBkMode(dc, TRANSPARENT);
    float sx = (float)(rc->right - rc->left) / 1024.0f, sy = (float)(rc->bottom - rc->top) / 768.0f;
#define GX(x) ((int)(((x) + 512) * sx))
#define GY(y) ((int)(((y) + 384) * sy))
    int connected = g_info.state == INFO_CONNECTED;

    if (!g_screen_open) {
        if (!connected && g_state == S_IDLE) return;
        char s[200];
        int paused = g_state == S_RUNNING && *(int *)(GAME + 0xB68) == 2 && *(int *)(GAME + 0xB80) == 3;
        if (paused && g_pause_by)
            snprintf(s, sizeof s, "Пауза: нажал %s (Esc - продолжить)", g_pause_by == g_role ? "ты" : g_pause_by == ROLE_HOST ? "хост" : "друг");
        else if (g_state == S_RUNNING)
            snprintf(s, sizeof s, "Кооп: пинг %u мс, задержка %u кадр. (%u мс), ожидание друга %u%%",
                     g_info.rtt_ms, g_delay, g_delay * 1000 / 60, g_wait_pct);
        else
            snprintf(s, sizeof s, "Кооп: соединено, пинг %u мс", g_info.rtt_ms);
        HFONT f = CreateFontW((int)(16 * sy), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ o = SelectObject(dc, f);
        RECT box = { 8, 8, 8 + (int)(470 * sx), 8 + (int)(26 * sy) };
        HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20));
        FillRect(dc, &box, bg);
        DeleteObject(bg);
        /* green: fine, yellow: noticeable, red: the game stutters */
        int bad = g_info.rtt_ms > 200 || g_wait_pct > 15, meh = g_info.rtt_ms > 100 || g_wait_pct > 3;
        SetTextColor(dc, paused ? RGB(220, 220, 220) : bad ? RGB(240, 70, 60) : meh ? RGB(240, 210, 60) : RGB(30, 235, 20));
        text_w(dc, box.left + 10, box.top + (int)(4 * sy), s, 0);
        SelectObject(dc, o);
        DeleteObject(f);
        return;
    }

    /* the game's look (Images/Interface m_server_list, m_message): near-black panel, thin
       light rounded frame, bright green condensed bold captions, menu items as plain text */
    char lines[6][200];
    button b[8];
    int nb, n = screen_content(lines, b, &nb);
    int h = screen_height(n, nb);
    RECT panel = { GX(-SCR_W / 2), GY(-h / 2), GX(SCR_W / 2), GY(h / 2) };
    HBRUSH bg = CreateSolidBrush(RGB(12, 12, 20));
    HPEN frame = CreatePen(PS_SOLID, sy > 1.5f ? 3 : 2, RGB(205, 205, 210));
    HGDIOBJ ob = SelectObject(dc, bg), op = SelectObject(dc, frame);
    RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, (int)(18 * sx), (int)(18 * sy));
    int sep = GY(-h / 2 + 58);
    MoveToEx(dc, panel.left, sep, NULL);
    LineTo(dc, panel.right, sep);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(bg);
    DeleteObject(frame);

    HFONT title = CreateFontW((int)(38 * sy), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Impact");
    HFONT body = CreateFontW((int)(19 * sy), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Arial");
    HFONT item = CreateFontW((int)(30 * sy), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Impact");
    HGDIOBJ old = SelectObject(dc, title);
    SetTextColor(dc, RGB(30, 235, 20));
    text_w(dc, panel.left, GY(-h / 2 + 10), "КООПЕРАТИВ ПО СЕТИ", panel.right - panel.left);
    SelectObject(dc, body);
    SetTextColor(dc, RGB(215, 215, 220));
    for (int i = 0; i < n; i++) text_w(dc, panel.left, GY(-h / 2 + 70 + i * 30), lines[i], panel.right - panel.left);

    float cx = 0, cy = 0;
    if (g_input_obj) { cx = *(float *)((uint8_t *)g_input_obj + 0x26C); cy = *(float *)((uint8_t *)g_input_obj + 0x270); }
    SelectObject(dc, item);
    for (int i = 0; i < nb; i++) {
        RECT g = button_rect(n, nb, i);
        int hover = cx >= g.left && cx < g.right && cy >= g.top && cy < g.bottom;
        RECT r = { GX(g.left), GY(g.top), GX(g.right), GY(g.bottom) };
        SetTextColor(dc, hover ? RGB(255, 255, 255) : RGB(30, 235, 20));
        text_w(dc, r.left, r.top + (int)(3 * sy), b[i].label, r.right - r.left);
    }
    SelectObject(dc, old);
    DeleteObject(title);
    DeleteObject(body);
    DeleteObject(item);
#undef GX
#undef GY
}

static LRESULT CALLBACK overlay_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        /* double buffered: the screen is redrawn often for the cursor */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        paint_overlay(mem, &rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_TIMER) {
        DWORD fg_pid = 0;
        HWND fg = GetForegroundWindow();
        GetWindowThreadProcessId(fg, &fg_pid);
        int visible = g_screen_open || g_info.state == INFO_CONNECTED || g_state != S_IDLE;
        if (visible && fg_pid == GetCurrentProcessId() && fg != w && !IsIconic(fg)) {
            RECT r;
            GetWindowRect(fg, &r); /* the game window (the first window found may be another one) */
            SetWindowPos(w, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(w, NULL, FALSE);
        } else ShowWindow(w, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static DWORD WINAPI overlay_thread(LPVOID p) {
    (void)p;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"rip3coop_overlay";
    RegisterClassW(&wc);
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"", WS_POPUP, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(g_overlay, OV_KEY, 205, LWA_COLORKEY | LWA_ALPHA); /* the game's cursor shows through */
    SetTimer(g_overlay, 1, 33, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}

/* A small real window for the language switch. Unlike the click-through co-op
   overlay it receives mouse clicks directly, so no game-cursor coordinates. */
static HWND g_lang_wnd;

static LRESULT CALLBACK lang_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_LBUTTONDOWN) {
        logf_("language window click");
        g_lang_ru = !g_lang_ru;
        if (save_language()) restart_game();
        return 0;
    }
    if (m == WM_TIMER) {
        static int shown;
        HWND game = FindWindowA(NULL, "RIP3: The Last Hero");
        if (game && main_menu_visible() && !g_screen_open) {
            RECT r;
            GetWindowRect(game, &r);
            SetWindowPos(w, HWND_TOPMOST, r.right - 264, r.bottom - 72, 240, 44,
                         SWP_SHOWWINDOW);
            if (!shown) { shown = 1; logf_("language window shown"); }
            InvalidateRect(w, NULL, FALSE);
        } else {
            if (shown) { shown = 0; logf_("language window hidden"); }
            ShowWindow(w, SW_HIDE);
        }
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(12, 12, 20));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(205, 205, 210));
        HGDIOBJ op = SelectObject(dc, pen);
        HBRUSH old = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 1, 1, rc.right - 1, rc.bottom - 1);
        SelectObject(dc, old);
        SelectObject(dc, op);
        DeleteObject(pen);
        SetBkMode(dc, TRANSPARENT);
        HFONT f = CreateFontW(18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f);
        SetTextColor(dc, RGB(30, 235, 20));
        char s[32];
        snprintf(s, sizeof s, "Язык: %s", g_lang_ru ? "RU" : "EN");
        text_w(dc, 0, 10, s, rc.right);
        SelectObject(dc, of);
        DeleteObject(f);
        EndPaint(w, &ps);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static DWORD WINAPI lang_thread(LPVOID p) {
    (void)p;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = lang_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"rip3coop_language";
    RegisterClassW(&wc);
    g_lang_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                 wc.lpszClassName, L"", WS_POPUP, 0, 0, 240, 44,
                                 NULL, NULL, wc.hInstance, NULL);
    SetTimer(g_lang_wnd, 1, 250, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}

static void __fastcall my_frame(void *game) {
    net_poll();
    DWORD fg_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    screen_keys(fg_pid == GetCurrentProcessId());
    if (key_edge(VK_F8) && fg_pid == GetCurrentProcessId() && g_role == ROLE_HOST) {
        if (g_state == S_IDLE) {
            if (!g_peer_connected) { status("Друг ещё не подключён"); g_screen_open = 1; }
            else start_host();
        } else {
            send_stop();
            end_session("хост нажал F8");
        }
    }
    if (g_state == S_STARTING && GetTickCount() - g_last_chunk_tick > 150) send_start_chunks();
    if (g_state == S_RUNNING && g_role == ROLE_HOST && g_resync_id && g_resync_have != RESYNC_ALL &&
        GetTickCount() - g_last_resync_tick > 150 && g_resync_sends < 60)
        send_resync_chunks();
    if (g_state != S_RUNNING) { orig_frame(game); return; }

    DWORD now = GetTickCount();
    if (now - g_last_peer_tick > PEER_TIMEOUT_MS) {
        send_stop();
        end_session("нет связи с другом 30 секунд");
        orig_frame(game);
        return;
    }
    if (!g_link_lost && now - g_last_peer_tick > 5000) {
        g_link_lost = 1;
        status("Связь с другом пропала. Жду до 30 секунд...");
    }
    if (!g_link_lost && now - g_last_status_tick > 5000) {
        status("кадр %u, ожиданий друга %u, задержка %u", g_frame, g_stalls, g_delay);
        g_last_status_tick = now;
    }
    /* share of the last second spent waiting for the friend (shown in the status line) */
    static DWORD wait_since, wait_ms, wait_from;
    if (now - wait_from >= 1000) { g_wait_pct = wait_ms * 100 / (now - wait_from ? now - wait_from : 1); wait_ms = 0; wait_from = now; }
    if (g_frame >= g_have_remote) {
        /* the peer's input for this frame has not arrived yet: wait, keep the window alive */
        if (!wait_since) wait_since = now;
        g_stalls++;
        if (now - g_last_send_tick >= 8) send_input();
        Sleep(1);
        return;
    }
    if (wait_since) { wait_ms += now - wait_since; wait_since = 0; }

    /* the host's window messages of this frame, on both sides */
    const np_frame *h = g_role == ROLE_HOST ? &g_local[g_frame % NP_RING] : &g_remote[g_frame % NP_RING];
    const np_frame *cf = g_role == ROLE_HOST ? &g_remote[g_frame % NP_RING] : &g_local[g_frame % NP_RING];
    g_share_ui = shared_ui((uint8_t *)game);
    g_in_dispatch = 1;
    for (int i = 0; i < h->nmsg; i++) {
        const np_msg *m = &h->msgs[i];
        if (m->msg == MSG_MOUSE_ITEM) continue;
        if (m->msg == WM_KEYDOWN && m->wparam == VK_ESCAPE) g_pause_by = ROLE_HOST;
        orig_msgproc(game, NULL, m->msg, m->wparam, m->lparam);
    }
    for (int i = 0; i < cf->nmsg; i++) {
        const np_msg *m = &cf->msgs[i];
        /* the friend's Esc pauses both games, also in a level */
        int esc = (m->msg == WM_KEYDOWN || m->msg == WM_KEYUP) && m->wparam == VK_ESCAPE;
        if (m->msg == MSG_MOUSE_ITEM || !(g_share_ui || esc)) continue;
        if (esc && m->msg == WM_KEYDOWN) g_pause_by = ROLE_CLIENT;
        orig_msgproc(game, NULL, m->msg, m->wparam, m->lparam);
    }
    g_in_dispatch = 0;

    uint32_t calls0 = g_rand_calls;
    orig_frame(game);
    if (g_state != S_RUNNING) return; /* stopped inside the frame (window closed) */
    check_slot *s = &g_my_checks[g_frame % NP_RING];
    s->frame = g_frame; s->seed = g_seed; s->calls = (uint16_t)(g_rand_calls - calls0); s->valid = 1;
    compare_checks(g_frame);
    g_frame++;
    send_input();
}

/* the main loop runs frames only while the window is active; a session must not pause */
static DWORD WINAPI keep_active(LPVOID p) {
    (void)p;
    for (;;) {
        if (g_state != S_IDLE) *(volatile uint8_t *)ADDR_ACTIVE = 1;
        Sleep(2);
    }
}

/* ---- install ---- */

static BOOL CALLBACK find_own_window(HWND w, LPARAM out) {
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    *(HWND *)out = w;
    return FALSE;
}

static DWORD WINAPI install(LPVOID self) {
    char path[MAX_PATH];
    GetModuleFileNameA((HMODULE)self, g_dir, sizeof g_dir);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\rip3net.log", g_dir);
    g_log = fopen(path, "w");
    snprintf(g_coop_save, sizeof g_coop_save, "%s\\coop_save.dat", g_dir);
    load_language();
    QueryPerformanceFrequency(&g_qpf);

    /* PECompact checks the unpacked code: patch only after the game made its window */
    HWND w = NULL;
    for (DWORD t0 = GetTickCount(); !w; Sleep(20)) {
        if (GetTickCount() - t0 > 120000) { logf_("no game window, nothing patched"); return 0; }
        EnumWindows(find_own_window, (LPARAM)&w);
    }
    if (*(uint8_t *)ADDR_RAND == 0xE9 || *(uint8_t *)ADDR_SRAND == 0xE9) {
        logf_("rand already hooked (rip3hook.dll loaded?), rip3net stays off");
        return 0;
    }
    int ok = jmp_to(ADDR_RAND, my_rand) && jmp_to(ADDR_SRAND, my_srand);
    int nf = redirect_calls(ADDR_MAINLOOP, 0x1A2, ADDR_FRAME, my_frame);
    int nt = redirect_calls(ADDR_FRAME, 0x32, ADDR_TIMER, my_timer);
    int np = redirect_calls(ADDR_INPUTSTEP, 0x40, ADDR_INPUT_POLL, my_poll);
    int nm = redirect_calls(ADDR_WNDPROC, 0x40, ADDR_MSGPROC, my_msgproc);
    int nl = redirect_calls(0x00401000u, 0x000B6000u, ADDR_LEVEL_LOAD, my_level_load);
    logf_("level load calls hooked: %d", nl);
    logf_("installed rand/srand=%d frame=%d timer=%d poll=%d msg=%d", ok, nf, nt, np, nm);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(NP_PORT_DLL);
    if (bind(g_sock, (struct sockaddr *)&a, sizeof a) != 0) {
        logf_("bind 127.0.0.1:%d failed (%d), no co-op", NP_PORT_DLL, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return 0;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    CreateThread(NULL, 0, keep_active, NULL, 0, NULL);
    CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, lang_thread, NULL, 0, NULL);
    logf_("MULTIPLAYER button -> co-op screen: %d", hook_multiplayer_button());
    logf_("listening on 127.0.0.1:%d", NP_PORT_DLL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(NULL, 0, install, h, 0, NULL);
    }
    return TRUE;
}
