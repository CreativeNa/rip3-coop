// rip3hook.dll - stage 2 desync probe for RIP 3 (exe SHA-256 C4305AA6...D925D2).
// Injected into the Steam-launched game. It:
//   - replaces CRT rand/srand with our own LCG (same algorithm) so the seed is ours
//   - forces a fixed timestep (60 Hz) in the frame timer
//   - records or replays the DirectInput state every frame
//   - records or replays the window messages that reach the game's message handler
//     (menu clicks and the fire flags come from WM_LBUTTONDOWN etc., not DirectInput)
//   - logs a per-frame state hash (game object floats + RNG seed + rand call count)
// Hotkeys: F9 = start recording, F10 = start replay of the last recording, F11 = stop.
// All addresses are for this exact build; see notes/stage1-lockstep.md.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

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
#define KBD_STATE_FIRST  0x0Fu        /* keyboard object at input+8, 256-byte state at +7 */
#define KBD_STATE_END    0x10Fu

#define FIXED_HZ         60.0
#define GAME_OBJ_SIZE    0x17000u
#define INPUT_OBJ_SIZE   0x7D4u
#define FIXED_SEED       0x1234567u

enum { MODE_IDLE, MODE_RECORD, MODE_REPLAY };

static int g_mode = MODE_IDLE;
static uint32_t g_seed = 1;
static uint32_t g_rand_calls;
static uint32_t g_frame;
static void *g_input_obj;
static uint8_t g_input_mask[INPUT_OBJ_SIZE]; /* 1 = data byte to record/replay */
static FILE *g_input_file, *g_hash_file, *g_msg_file, *g_log;
typedef struct { uint32_t frame, msg, wparam, lparam; } msg_rec;
static msg_rec g_next_msg;      /* replay: next recorded message not yet dispatched */
static int g_have_next_msg;
static int g_in_replay_dispatch;
static char g_dir[MAX_PATH];
static LARGE_INTEGER g_qpf, g_last_frame;

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

/* ---- RNG (same LCG as MSVC CRT, but with a seed we own) ---- */

static int __cdecl my_rand(void) {
    g_rand_calls++;
    g_seed = g_seed * 0x343FDu + 0x269EC3u;
    return (int)((g_seed >> 16) & 0x7FFF);
}

static void __cdecl my_srand(unsigned s) {
    logf_("srand(%u) frame=%u", s, g_frame);
    if (g_mode == MODE_IDLE) g_seed = s;
}

/* ---- fixed timestep ---- */

static void __fastcall my_timer(float *t, void *edx, int cap) {
    (void)edx;
    /* pace to FIXED_HZ in real time so the game does not run too fast */
    LARGE_INTEGER now;
    double period = (double)g_qpf.QuadPart / FIXED_HZ;
    for (;;) {
        QueryPerformanceCounter(&now);
        double el = (double)(now.QuadPart - g_last_frame.QuadPart);
        if (el >= period) break;
        if (period - el > g_qpf.QuadPart / 1000.0 * 2) Sleep(1);
    }
    g_last_frame = now;
    orig_timer(t, edx, cap);
    if (g_mode == MODE_IDLE) return;
    /* overwrite what FUN_0043efb0 derived from the real clock */
    float dt = (float)(1.0 / FIXED_HZ) * t[1];
    t[2] = t[3] = t[4] = t[5] = t[6] = dt;
    float avg = (float)((dt * 4.0f) * *(double *)ADDR_TIMER_K);
    t[9] = avg;
    t[8] = avg * t[0];
    t[7] = t[1] / t[8];
}

/* ---- input record/replay ---- */

static void build_input_mask(uint8_t *obj) {
    /* everything is data except dwords that point into committed memory */
    memset(g_input_mask, 1, sizeof g_input_mask);
    for (uint32_t off = 0; off + 4 <= INPUT_OBJ_SIZE; off += 4) {
        uint32_t v = *(uint32_t *)(obj + off);
        MEMORY_BASIC_INFORMATION mbi;
        /* flag bytes like 01 01 01 00 (keyboard valid/acquired at +0x0C..0x0E) are
           small values that happen to land in committed memory: not pointers */
        if (v >= 0x400000 && VirtualQuery((void *)v, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
            memset(g_input_mask + off, 0, 4);
    }
    /* key bytes are never pointers (a held F9 looked like one) */
    memset(g_input_mask + KBD_STATE_FIRST, 1, KBD_STATE_END - KBD_STATE_FIRST);
}

/* ---- window messages ---- */

static int __fastcall my_msgproc(void *game, void *edx, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    if (g_mode == MODE_RECORD && g_msg_file) {
        msg_rec m = { g_frame, msg, wparam, lparam };
        fwrite(&m, sizeof m, 1, g_msg_file);
    } else if (g_mode == MODE_REPLAY && !g_in_replay_dispatch && msg != WM_DESTROY && msg != WM_CLOSE) {
        return 0; /* the recorded stream replaces the real one */
    }
    return orig_msgproc(game, edx, msg, wparam, lparam);
}

/* replay: feed the messages recorded before frame g_frame */
static void dispatch_recorded_messages(void *game) {
    while (g_have_next_msg && g_next_msg.frame <= g_frame) {
        if (g_next_msg.frame < g_frame) logf_("replay: late message %x for frame %u", g_next_msg.msg, g_next_msg.frame);
        g_in_replay_dispatch = 1;
        orig_msgproc(game, NULL, g_next_msg.msg, g_next_msg.wparam, g_next_msg.lparam);
        g_in_replay_dispatch = 0;
        g_have_next_msg = g_msg_file && fread(&g_next_msg, sizeof g_next_msg, 1, g_msg_file) == 1;
    }
}

static void __fastcall my_poll(void *input) {
    g_input_obj = input;
    orig_poll(input);
    uint8_t *obj = (uint8_t *)input;
    if (g_mode == MODE_RECORD && g_input_file) {
        fwrite(&g_frame, 4, 1, g_input_file);
        fwrite(obj, INPUT_OBJ_SIZE, 1, g_input_file);
    } else if (g_mode == MODE_REPLAY && g_input_file) {
        uint32_t f;
        static uint8_t buf[INPUT_OBJ_SIZE];
        if (fread(&f, 4, 1, g_input_file) != 1 || fread(buf, INPUT_OBJ_SIZE, 1, g_input_file) != 1) {
            logf_("replay: end of input at frame %u", g_frame);
            fclose(g_input_file);
            g_input_file = NULL;
            return;
        }
        if (f != g_frame) logf_("replay: frame mismatch file=%u game=%u", f, g_frame);
        for (uint32_t i = 0; i < INPUT_OBJ_SIZE; i++)
            if (g_input_mask[i]) obj[i] = buf[i];
    }
}

/* ---- state hash ---- */

static uint32_t hash_game(const uint8_t *game) {
    uint32_t h = 2166136261u;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(game, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    uint32_t end = (uint32_t)mbi.BaseAddress + mbi.RegionSize;
    uint32_t n = GAME_OBJ_SIZE;
    if ((uint32_t)game + n > end) n = end - (uint32_t)game;
    for (uint32_t off = 0; off + 4 <= n; off += 4) {
        float f = *(const float *)(game + off);
        float a = f < 0 ? -f : f;
        /* skip pointers/handles: as floats they are denormal-tiny or huge */
        if (!(f == 0.0f || (a > 1e-6f && a < 1e7f))) continue;
        uint32_t v = *(const uint32_t *)(game + off);
        for (int k = 0; k < 4; k++) { h ^= (v >> (k * 8)) & 0xFF; h *= 16777619u; }
    }
    return h;
}

/* ---- modes ---- */

static void stop_mode(void) {
    if (g_input_file) fclose(g_input_file);
    if (g_hash_file) fclose(g_hash_file);
    if (g_msg_file) fclose(g_msg_file);
    g_input_file = g_hash_file = g_msg_file = NULL;
    g_have_next_msg = 0;
    logf_("stop mode=%d frames=%u", g_mode, g_frame);
    g_mode = MODE_IDLE;
}

static void start_mode(int mode) {
    char path[MAX_PATH];
    stop_mode();
    g_frame = 0;
    g_rand_calls = 0;
    g_seed = FIXED_SEED;
    if (mode == MODE_RECORD) {
        if (g_input_obj) build_input_mask((uint8_t *)g_input_obj);
        snprintf(path, sizeof path, "%s\\input.bin", g_dir);
        g_input_file = fopen(path, "wb");
        snprintf(path, sizeof path, "%s\\mask.bin", g_dir);
        FILE *m = fopen(path, "wb");
        if (m) { fwrite(g_input_mask, sizeof g_input_mask, 1, m); fclose(m); }
        snprintf(path, sizeof path, "%s\\msgs.bin", g_dir);
        g_msg_file = fopen(path, "wb");
        snprintf(path, sizeof path, "%s\\hash_record.txt", g_dir);
    } else {
        snprintf(path, sizeof path, "%s\\msgs.bin", g_dir);
        g_msg_file = fopen(path, "rb");
        g_have_next_msg = g_msg_file && fread(&g_next_msg, sizeof g_next_msg, 1, g_msg_file) == 1;
        snprintf(path, sizeof path, "%s\\mask.bin", g_dir);
        FILE *m = fopen(path, "rb");
        if (m) { fread(g_input_mask, sizeof g_input_mask, 1, m); fclose(m); }
        snprintf(path, sizeof path, "%s\\input.bin", g_dir);
        g_input_file = fopen(path, "rb");
        snprintf(path, sizeof path, "%s\\hash_replay.txt", g_dir);
    }
    g_hash_file = fopen(path, "w");
    g_mode = mode;
    logf_("start mode=%d input=%p ok=%d", mode, g_input_obj, g_input_file != NULL);
}

static int key_edge(int vk) {
    static SHORT prev[256];
    SHORT s = GetAsyncKeyState(vk);
    int down = (s & 0x8000) && !(prev[vk] & 0x8000);
    prev[vk] = s;
    return down;
}

/* Logs the sound manager state: FUN_0043b0c0 (is-playing) dereferences sound
   slots without checking the "sound enabled" flag at +0x884. */
static void log_sound_state(uint8_t *game) {
    uint8_t *snd = *(uint8_t **)(game + 0x155E8);
    if (!snd) { logf_("sound: manager is NULL"); return; }
    int nulls = 0, first_null = -1;
    for (int i = 0; i < 0x80; i++) {
        if (*(void **)(snd + 0x890 + i * 4) == NULL) { if (first_null < 0) first_null = i; nulls++; }
    }
    logf_("sound: game=%p mgr=%p enabled=%u null_slots(0..127)=%d first_null=%d slot17=%p",
          game, snd, snd[0x884], nulls, first_null, *(void **)(snd + 0x890 + 0x17 * 4));
}

static void __fastcall my_frame(void *game) {
    static uint32_t total;
    if (total++ % 600 == 0) log_sound_state((uint8_t *)game);
    if (key_edge(VK_F9)) start_mode(MODE_RECORD);
    if (key_edge(VK_F10)) start_mode(MODE_REPLAY);
    if (key_edge(VK_F11)) stop_mode();
    if (g_mode == MODE_REPLAY) dispatch_recorded_messages(game);
    uint32_t calls0 = g_rand_calls;
    orig_frame(game);
    if (g_mode != MODE_IDLE && g_hash_file) {
        fprintf(g_hash_file, "%u %08x %08x %u\n", g_frame, hash_game((uint8_t *)game), g_seed,
                g_rand_calls - calls0);
    }
    g_frame++;
}

/* ---- install ---- */

static DWORD WINAPI install(LPVOID self) {
    char path[MAX_PATH];
    GetModuleFileNameA((HMODULE)self, path, sizeof path);
    char *slash = strrchr(path, '\\');
    if (slash) *slash = 0;
    snprintf(g_dir, sizeof g_dir, "%s\\..\\local\\runs", path);
    CreateDirectoryA(g_dir, NULL);
    snprintf(path, sizeof path, "%s\\hook.log", g_dir);
    g_log = fopen(path, "a");
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_last_frame);

    /* sanity: only patch the build we analysed */
    if (*(uint8_t *)ADDR_RAND == 0xE9 && *(uint8_t *)ADDR_SRAND == 0xE9) {
        logf_("already installed");
        return 0;
    }
    int ok = jmp_to(ADDR_RAND, my_rand) && jmp_to(ADDR_SRAND, my_srand);
    int nf = redirect_calls(ADDR_MAINLOOP, 0x1A2, ADDR_FRAME, my_frame);
    int nt = redirect_calls(ADDR_FRAME, 0x32, ADDR_TIMER, my_timer);
    int np = redirect_calls(ADDR_INPUTSTEP, 0x40, ADDR_INPUT_POLL, my_poll);
    int nm = redirect_calls(ADDR_WNDPROC, 0x40, ADDR_MSGPROC, my_msgproc);
    logf_("installed rand/srand=%d frame_calls=%d timer_calls=%d poll_calls=%d msg_calls=%d", ok, nf, nt, np, nm);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        install(h);
    }
    return TRUE;
}
