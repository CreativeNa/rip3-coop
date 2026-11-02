// rip3video.dll - modern display for RIP 3 (exe SHA-256 C4305AA6...D925D2).
// The game switches the monitor to 1024x768 fullscreen, which rearranges the
// desktop. Instead: borderless window (or a normal window) on the desktop's own
// resolution, the 1024x768 frame upscaled to fit (keeping 4:3) with a Catmull-Rom
// filter + AMD RCAS sharpening (FSR 1 style), optional vsync, DPI aware.
// Only the import table (data) and D3D9 vtables are patched, never game code:
// PECompact checks the code after unpacking. Must be injected right after launch.
// Settings: rip3video.ini next to the DLL.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "upscale_shaders.h"

static int g_profile;            /* startup profiler, see profiler() */
static int g_fast_zip;           /* data.pak from memory, see fast_zip() */
static DWORD g_main_tid;
static DWORD WINAPI profiler(LPVOID p);

/* the game's own IAT and the packer's copy of it */
static const uint32_t k_slots_d3dcreate[] = { 0x004B9294u, 0x004EFE78u };
static const uint32_t k_slots_createwnd[] = { 0x004B9254u, 0x004EFE38u };
#define GAME_W 1024
#define GAME_H 768
/* the game's frame limiter: FUN_0043efb0 spins while fps > this value (int) */
#define ADDR_FPS_CAP (0x00503B50u + 0x155D8u)
#define WINDOW_TITLE "RIP3: The Last Hero"

enum { MODE_BORDERLESS, MODE_WINDOW };
enum { SCALE_FIT, SCALE_STRETCH, SCALE_INTEGER };
enum { UPSCALE_BILINEAR, UPSCALE_FSR, UPSCALE_SHARP };

static int g_mode = MODE_BORDERLESS, g_scale = SCALE_FIT, g_vsync = 1, g_win_scale = 1, g_fps_cap = 0;
static int g_upscale = UPSCALE_FSR;
static float g_sharpness = 0.2f;
static HWND g_hwnd;
static FILE *g_log;

typedef HWND(WINAPI *CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT(WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT(WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);

static CreateWindowExA_t real_CreateWindowExA;
static Direct3DCreate9_t real_Direct3DCreate9;
static CreateDevice_t real_CreateDevice;
static Reset_t real_Reset;
static Present_t real_Present;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void write_ptr(void *at, void *value) {
    DWORD old;
    VirtualProtect(at, sizeof(void *), PAGE_READWRITE, &old);
    *(void **)at = value;
    VirtualProtect(at, sizeof(void *), old, &old);
}

/* replaces method `index` of a COM object's vtable, returns the original */
static void *hook_vtable(void *obj, int index, void *fn) {
    void **vt = *(void ***)obj;
    void *orig = vt[index];
    if (orig != fn) write_ptr(&vt[index], fn);
    return orig == fn ? NULL : orig;
}

static RECT monitor_rect(HWND near_wnd) {
    MONITORINFO mi = { sizeof mi };
    HMONITOR m = near_wnd ? MonitorFromWindow(near_wnd, MONITOR_DEFAULTTOPRIMARY)
                          : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoA(m, &mi);
    return mi.rcMonitor;
}

/* ---- publisher/developer logos ----
   FUN_004138a0 shows them right after creating the window:
   0x413DB4 call FUN_00403d80 (Images/meridian4), 0x413DBB call FUN_00406db0 (Images/logo).
   Both are self-contained fade loops; the calls are pointed at an empty function.
   Done from CreateWindowExA: the code is unpacked and checked by then, and the
   logos need the window, so they have not run yet. */

static const struct { uint32_t site, target; } k_logo_calls[] = {
    { 0x00413DB4u, 0x00403D80u }, { 0x00413DBBu, 0x00406DB0u },
};
static int g_skip_logos = 1;

static void __fastcall no_logo(void *game) { (void)game; }

static void skip_logos(void) {
    for (size_t i = 0; i < sizeof k_logo_calls / sizeof k_logo_calls[0]; i++) {
        uint8_t *p = (uint8_t *)k_logo_calls[i].site;
        if (p[0] != 0xE8 || k_logo_calls[i].site + 5 + *(int32_t *)(p + 1) != k_logo_calls[i].target) {
            logf_("logo call %08x not found, logos kept", k_logo_calls[i].site);
            return;
        }
    }
    for (size_t i = 0; i < sizeof k_logo_calls / sizeof k_logo_calls[0]; i++) {
        uint8_t *p = (uint8_t *)k_logo_calls[i].site;
        int32_t rel = (int32_t)((uint32_t)no_logo - (k_logo_calls[i].site + 5));
        DWORD old;
        VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t *)(p + 1) = rel;
        VirtualProtect(p + 1, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), (void *)0x00413DB4u, 16);
    logf_("logos skipped");
}

/* ---- missing audio device ----
   Sound manager = *(game+0x155E8): +0x884 enabled, +0x88C music, +0x890 128 sound slots
   (audiere OutputStream*). Without an output device (headphones unplugged) the slots stay
   NULL, and FUN_0043b0c0 (is the sound playing?) dereferences them with no check at all;
   play/stop check only "enabled". Fix: FUN_0043b0c0 checks, and empty slots get a silent
   stream whose methods do nothing. */

#define ADDR_GAME_SOUND  0x00503B50u + 0x155E8u
#define ADDR_IS_PLAYING  0x0043B0C0u

static int __stdcall silent_1(void *s) { (void)s; return 0; }
static int __stdcall silent_2(void *s, int a) { (void)s; (void)a; return 0; }
static float __stdcall silent_getf(void *s) { (void)s; return 0.0f; }
/* audiere OutputStream: ref, unref, play, stop, isPlaying, reset, setRepeat, getRepeat,
   setVolume, getVolume, setPan, getPan, setPitchShift, getPitchShift, isSeekable,
   getLength, setPosition, getPosition */
static void *g_silent_vtbl[24] = {
    silent_1, silent_1, silent_1, silent_1, silent_1, silent_1, silent_2, silent_1,
    silent_2, silent_getf, silent_2, silent_getf, silent_2, silent_getf, silent_1, silent_1,
    silent_2, silent_1, silent_1, silent_1, silent_1, silent_1, silent_1, silent_1,
};
static void *g_silent_stream = g_silent_vtbl;

static char __fastcall safe_is_playing(uint8_t *mgr, void *edx, int idx) {
    (void)edx;
    if (!mgr || idx < 0 || idx >= 128) return 0;
    void **slot = *(void ***)(mgr + 0x890 + idx * 4);
    if (!slot || (void *)slot == (void *)&g_silent_stream) return 0;
    return (char)((int(__stdcall *)(void *))(*(void ***)slot)[4])(slot);
}

static DWORD WINAPI audio_guard(LPVOID p) {
    (void)p;
    for (;;) {
        uint8_t *mgr = *(uint8_t **)ADDR_GAME_SOUND;
        if (mgr) {
            for (int i = 0; i < 128; i++)
                if (!*(void **)(mgr + 0x890 + i * 4)) *(void **)(mgr + 0x890 + i * 4) = &g_silent_stream;
            if (!*(void **)(mgr + 0x88C)) *(void **)(mgr + 0x88C) = &g_silent_stream;
        }
        Sleep(5);
    }
}

/* ---- faster loading: data.pak in memory ----
   The game reads data.pak with XUnzip. Its stream (0x20 bytes): +0 on a file handle, +1 can
   seek, +4 handle, +8 error, +0x0C start offset, +0x10 close the handle, +0x14 memory buffer,
   +0x18 length, +0x1C position. Every file lookup walks the whole zip directory, and on a
   file handle that is one ReadFile per byte (FUN_00442fd0): most of the loading time.
   XUnzip also reads from memory, so read/seek/tell are replaced, and a stream on a file is
   switched to memory on first use, with the whole file read once and kept. */

#define ADDR_ZREAD 0x00442F50u
#define ADDR_ZSEEK 0x00442EC0u
#define ADDR_ZTELL 0x00442E90u

typedef struct {
    uint8_t on_file, can_seek, pad[2];
    HANDLE h;
    uint8_t err, pad2[3];
    uint32_t start;
    uint8_t close_h, pad3[3];
    uint8_t *buf;
    uint32_t len, pos;
} zstream;

typedef struct { DWORD vol, hi, lo, size; uint8_t *data; } zcache;
static zcache g_zcache[8];
static int g_zcache_n;

static void zs_to_memory(zstream *s) {
    static int once;
    if (!once) { once = 1; logf_("zip stream: on file %d, can seek %d, length %lu", s->on_file, s->can_seek, (unsigned long)s->len); }
    if (!s->on_file || !s->can_seek) return;
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(s->h, &fi) || fi.nFileSizeHigh) return;
    uint8_t *data = NULL;
    for (int i = 0; i < g_zcache_n; i++)
        if (g_zcache[i].vol == fi.dwVolumeSerialNumber && g_zcache[i].hi == fi.nFileIndexHigh &&
            g_zcache[i].lo == fi.nFileIndexLow && g_zcache[i].size == fi.nFileSizeLow) data = g_zcache[i].data;
    DWORD cur = SetFilePointer(s->h, 0, NULL, FILE_CURRENT);
    if (cur == INVALID_SET_FILE_POINTER || cur < s->start || s->start > fi.nFileSizeLow) return;
    if (!data) {
        if (g_zcache_n == 8) return;
        data = (uint8_t *)malloc(fi.nFileSizeLow ? fi.nFileSizeLow : 1);
        DWORD got = 0;
        if (!data || SetFilePointer(s->h, 0, NULL, FILE_BEGIN) != 0 ||
            !ReadFile(s->h, data, fi.nFileSizeLow, &got, NULL) || got != fi.nFileSizeLow) {
            free(data);
            SetFilePointer(s->h, (LONG)cur, NULL, FILE_BEGIN);
            return;
        }
        g_zcache[g_zcache_n++] = (zcache){ fi.dwVolumeSerialNumber, fi.nFileIndexHigh, fi.nFileIndexLow, fi.nFileSizeLow, data };
        logf_("zip stream: %lu bytes now read from memory", fi.nFileSizeLow);
    }
    s->buf = data + s->start;
    s->len = fi.nFileSizeLow - s->start;
    s->pos = cur - s->start;
    s->on_file = 0;    /* the handle stays open; FUN_00442e40 closes it as before */
}

static uint32_t __cdecl z_read(void *dst, uint32_t size, uint32_t n, zstream *s) {
    zs_to_memory(s);
    uint32_t want = size * n;
    if (s->on_file) {
        DWORD got = 0;
        if (!ReadFile(s->h, dst, want, &got, NULL)) s->err = 1;
        return size ? got / size : 0;
    }
    if (s->pos >= s->len) want = 0;
    else if (s->len - s->pos < want) want = s->len - s->pos;
    memcpy(dst, s->buf + s->pos, want);
    s->pos += want;
    return size ? want / size : 0;
}

static uint32_t __cdecl z_seek(zstream *s, int32_t off, int how) {
    zs_to_memory(s);
    if (s->on_file) {
        if (!s->can_seek) return 0x1D;
        if (how == 0) SetFilePointer(s->h, (LONG)(s->start + off), NULL, FILE_BEGIN);
        else if (how == 1 || how == 2) SetFilePointer(s->h, off, NULL, how);
        else return 0x13;
        return 0;
    }
    if (how == 0) s->pos = off;
    else if (how == 1) s->pos += off;
    else if (how == 2) s->pos = s->len + off;
    return 0;
}

static uint32_t __cdecl z_tell(zstream *s) {
    zs_to_memory(s);
    if (!s->on_file) return s->pos;
    if (!s->can_seek) return 0;
    return SetFilePointer(s->h, 0, NULL, FILE_CURRENT) - s->start;
}

static void jmp_to(uint32_t at, void *to) {
    uint8_t b[5] = { 0xE9 };
    int32_t rel = (int32_t)((uint32_t)to - (at + 5));
    memcpy(b + 1, &rel, 4);
    DWORD old;
    VirtualProtect((void *)at, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)at, b, 5);
    VirtualProtect((void *)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 5);
}

/* Lookup by name (FUN_00443780, unzLocateFile) reads every directory entry until the name
   matches: thousands of entries per file. The unz object: +4 entry count, +0x10 current entry,
   +0x14 its directory offset, +0x18 entry valid, +0x24 first entry offset, +0x28/+0x78 entry
   info. An index of all names is built once per archive, then lookup jumps straight there. */
#define ADDR_ZLOCATE 0x00443780u
typedef int (__cdecl *unz_fn1)(uint8_t *unz);
typedef int (__cdecl *unz_name_fn)(uint8_t *unz, void *info, char *name, uint32_t size, void *, uint32_t, void *, uint32_t);
typedef int (__cdecl *unz_entry_fn)(uint8_t *unz, void *info, void *info2, void *, uint32_t, void *, uint32_t, void *, uint32_t);
typedef int (__cdecl *unz_cmp_fn)(const char *a, const char *b, int mode);
#define unz_first ((unz_fn1)0x004436C0u)
#define unz_next  ((unz_fn1)0x00443710u)
#define unz_name  ((unz_name_fn)0x00443680u)
#define unz_entry ((unz_entry_fn)0x00443330u)
#define unz_cmp   ((unz_cmp_fn)0x00443150u)

typedef struct { uint32_t idx, pos; char *name; } zentry;
typedef struct { uint8_t *unz; uint32_t count, first, cap; zentry *tab; } zindex;
static zindex g_zidx[4];

static uint32_t zhash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        char c = *s == '\\' ? '/' : *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    return h;
}

static zindex *zindex_for(uint8_t *unz) {
    uint32_t count = *(uint32_t *)(unz + 4), first = *(uint32_t *)(unz + 0x24);
    zindex *free_slot = NULL;
    for (int i = 0; i < 4; i++) {
        zindex *x = &g_zidx[i];
        if (x->unz == unz && x->count == count && x->first == first) return x;
        if (!x->unz && !free_slot) free_slot = x;
    }
    if (!free_slot) free_slot = &g_zidx[0];
    zindex *x = free_slot;
    if (x->tab) { for (uint32_t i = 0; i < x->cap; i++) free(x->tab[i].name); free(x->tab); }
    memset(x, 0, sizeof *x);
    uint32_t cap = 16;
    while (cap < count * 2 + 16) cap <<= 1;
    x->tab = (zentry *)calloc(cap, sizeof(zentry));
    if (!x->tab) return NULL;
    x->unz = unz; x->count = count; x->first = first; x->cap = cap;
    char name[260];
    uint32_t n = 0;
    for (int r = unz_first(unz); r == 0; r = unz_next(unz)) {
        unz_name(unz, NULL, name, 0x100, NULL, 0, NULL, 0);
        name[0x100] = 0;
        uint32_t h = zhash(name) & (cap - 1);
        while (x->tab[h].name) h = (h + 1) & (cap - 1);
        x->tab[h] = (zentry){ *(uint32_t *)(unz + 0x10), *(uint32_t *)(unz + 0x14), _strdup(name) };
        n++;
    }
    logf_("zip index: %lu files", (unsigned long)n);
    return x;
}

static int __cdecl z_locate(uint8_t *unz, const char *name, int mode) {
    if (!unz) return -0x66;
    if (strlen(name) > 0xFF) return -0x66;
    if (!*(uint32_t *)(unz + 0x18)) return -100;
    uint32_t cur_idx = *(uint32_t *)(unz + 0x10), cur_pos = *(uint32_t *)(unz + 0x14);
    zindex *x = zindex_for(unz);
    if (x) {
        for (uint32_t h = zhash(name) & (x->cap - 1); x->tab[h].name; h = (h + 1) & (x->cap - 1)) {
            if (unz_cmp(x->tab[h].name, name, mode) != 0) continue;
            *(uint32_t *)(unz + 0x10) = x->tab[h].idx;
            *(uint32_t *)(unz + 0x14) = x->tab[h].pos;
            int r = unz_entry(unz, unz + 0x28, unz + 0x78, NULL, 0, NULL, 0, NULL, 0);
            *(uint32_t *)(unz + 0x18) = r == 0;
            return r;
        }
    }
    /* not found: leave the current entry as it was, like the original */
    *(uint32_t *)(unz + 0x10) = cur_idx;
    *(uint32_t *)(unz + 0x14) = cur_pos;
    if (x) unz_entry(unz, unz + 0x28, unz + 0x78, NULL, 0, NULL, 0, NULL, 0);
    return -100;
}

/* The game's images are interlaced PNGs. libpng's png_do_read_interlace (0x473BF6) and
   png_combine_row (0x4739E0) move every pixel with a one-count "rep movs": a third of the
   loading time. Same results here, with plain copies. */
#define ADDR_PNG_INTERLACE 0x00473BF6u
#define ADDR_PNG_COMBINE   0x004739E0u
#define PNG_PASS_INC ((const int *)0x004C4748u)

typedef struct { uint32_t width, rowbytes; uint8_t color_type, bit_depth, channels, pixel_depth; } png_row_info;

/* both are __stdcall in the game (ret 0x10 / ret 0xc) */
static void __stdcall png_interlace(png_row_info *ri, uint8_t *row, int pass, uint32_t transformations) {
    (void)transformations;
    if (!row || !ri) return;
    int jump = PNG_PASS_INC[pass];
    uint32_t width = ri->width, final = width * jump, depth = ri->pixel_depth;
    if (depth == 1 || depth == 2 || depth == 4) {
        uint32_t ppb = 8 / depth, top = 8 - depth, bm = (1u << depth) - 1;
        uint8_t *sp = row + (width - 1) / ppb, *dp = row + (final - 1) / ppb;
        uint32_t ss = (ppb - 1 - (width - 1) % ppb) * depth, ds = (ppb - 1 - (final - 1) % ppb) * depth;
        for (uint32_t i = 0; i < width; i++) {
            uint32_t v = (*sp >> ss) & bm;
            for (int j = 0; j < jump; j++) {
                *dp = (uint8_t)((*dp & ~(bm << ds)) | (v << ds));
                if (ds == top) { ds = 0; dp--; } else ds += depth;
            }
            if (ss == top) { ss = 0; sp--; } else ss += depth;
        }
    } else {
        uint32_t pb = depth >> 3;
        if (width) {
            uint8_t *sp = row + (width - 1) * pb, *dp = row + (final - 1) * pb;
            if (pb == 4) {
                for (uint32_t i = 0; i < width; i++, sp -= 4) {
                    uint32_t v; memcpy(&v, sp, 4);
                    for (int j = 0; j < jump; j++, dp -= 4) memcpy(dp, &v, 4);
                }
            } else {
                uint8_t v[16];
                for (uint32_t i = 0; i < width; i++, sp -= pb) {
                    memcpy(v, sp, pb);
                    for (int j = 0; j < jump; j++, dp -= pb) memcpy(dp, v, pb);
                }
            }
        }
    }
    ri->width = final;
    ri->rowbytes = (ri->pixel_depth * final + 7) >> 3;
}

static void __stdcall png_combine(uint8_t *png, uint8_t *row, uint32_t mask) {
    uint32_t depth = png[0xFB], width = *(uint32_t *)(png + 0xB8);
    const uint8_t *sp = *(uint8_t **)(png + 0xDC) + 1;
    if (mask == 0xFF) { memcpy(row, sp, (depth * width + 7) >> 3); return; }
    if (depth == 1 || depth == 2 || depth == 4) {
        uint32_t top = 8 - depth, bm = (1u << depth) - 1, sh = top, m = 0x80;
        for (uint32_t i = 0; i < width; i++) {
            if (m & mask) *row = (uint8_t)((*row & ~(bm << sh)) | (((*sp >> sh) & bm) << sh));
            if (sh == 0) { sp++; row++; sh = top; } else sh -= depth;
            m = m == 1 ? 0x80 : m >> 1;
        }
        return;
    }
    uint32_t pb = depth >> 3, m = 0x80;
    for (uint32_t i = 0; i < width; i++, sp += pb, row += pb) {
        if (m & mask) {
            if (pb == 4) memcpy(row, sp, 4); else memcpy(row, sp, pb);
        }
        m = m == 1 ? 0x80 : m >> 1;
    }
}

static void fast_zip(void) {
    jmp_to(ADDR_PNG_INTERLACE, png_interlace);
    jmp_to(ADDR_PNG_COMBINE, png_combine);
    jmp_to(ADDR_ZREAD, z_read);
    jmp_to(ADDR_ZSEEK, z_seek);
    jmp_to(ADDR_ZTELL, z_tell);
    jmp_to(ADDR_ZLOCATE, z_locate);
    logf_("zip reads from memory");
}

static void fix_audio(void) {
    uint8_t b[5] = { 0xE9 };
    int32_t rel = (int32_t)((uint32_t)safe_is_playing - (ADDR_IS_PLAYING + 5));
    memcpy(b + 1, &rel, 4);
    DWORD old;
    VirtualProtect((void *)ADDR_IS_PLAYING, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)ADDR_IS_PLAYING, b, 5);
    VirtualProtect((void *)ADDR_IS_PLAYING, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)ADDR_IS_PLAYING, 5);
    CreateThread(NULL, 0, audio_guard, NULL, 0, NULL);
    logf_("audio guard on");
}

/* ---- window ---- */

/* Windows can reset a borderless window after Win+D, a display-mode change or a
   remote-session reconnect. Put it back on the monitor instead of leaving the game
   in a small window. Minimized windows are left alone. */
static DWORD WINAPI window_guard(LPVOID p) {
    (void)p;
    int was_bad = 0;
    for (;;) {
        Sleep(500);
        if (g_mode != MODE_BORDERLESS || !g_hwnd || !IsWindow(g_hwnd) || IsIconic(g_hwnd)) continue;
        RECT wr, mr = monitor_rect(g_hwnd);
        if (!GetWindowRect(g_hwnd, &wr)) continue;
        LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
        LONG ex = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
        int bad_rect = abs(wr.left - mr.left) > 1 || abs(wr.top - mr.top) > 1 ||
                       abs((wr.right - wr.left) - (mr.right - mr.left)) > 1 ||
                       abs((wr.bottom - wr.top) - (mr.bottom - mr.top)) > 1;
        int bad_style = (style & WS_POPUP) == 0 || (ex & WS_EX_TOPMOST) != 0;
        if (!bad_rect && !bad_style) { was_bad = 0; continue; }
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
        SetWindowPos(g_hwnd, HWND_TOP, mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        if (!was_bad) logf_("window reset: restored borderless %ldx%ld at %ld,%ld",
                            (long)(mr.right - mr.left), (long)(mr.bottom - mr.top), (long)mr.left, (long)mr.top);
        was_bad = 1;
    }
    return 0;
}

static HWND WINAPI my_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    if (!parent && title && !IsBadStringPtrA(title, 64) && strcmp(title, WINDOW_TITLE) == 0) {
        RECT mr = monitor_rect(NULL);
        if (g_mode == MODE_BORDERLESS) {
            ex &= ~(WS_EX_TOPMOST);
            style = WS_POPUP | WS_VISIBLE;
            x = mr.left; y = mr.top; w = mr.right - mr.left; h = mr.bottom - mr.top;
        } else {
            RECT r = { 0, 0, GAME_W * g_win_scale, GAME_H * g_win_scale };
            style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
            AdjustWindowRectEx(&r, style, FALSE, ex);
            w = r.right - r.left; h = r.bottom - r.top;
            x = mr.left + ((mr.right - mr.left) - w) / 2;
            y = mr.top + ((mr.bottom - mr.top) - h) / 2;
        }
        g_hwnd = real_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
        logf_("window %p mode=%d %dx%d at %d,%d", (void *)g_hwnd, g_mode, w, h, x, y);
        static int guard_started;
        if (g_hwnd && !guard_started) {
            guard_started = 1;
            CloseHandle(CreateThread(NULL, 0, window_guard, NULL, 0, NULL));
        }
        if (g_skip_logos) skip_logos();
        if (g_profile) { g_main_tid = GetCurrentThreadId(); CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL)); }
        fix_audio();
        if (g_fast_zip) fast_zip();
        return g_hwnd;
    }
    return real_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
}

/* ---- D3D ---- */

static void fix_params(D3DPRESENT_PARAMETERS *pp) {
    if (!pp->BackBufferWidth || !pp->BackBufferHeight) { pp->BackBufferWidth = GAME_W; pp->BackBufferHeight = GAME_H; }
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->BackBufferFormat = D3DFMT_UNKNOWN;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->PresentationInterval = g_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (g_hwnd) pp->hDeviceWindow = g_hwnd;
}

/* where the 1024x768 frame goes inside the client area */
static RECT dest_rect(HWND w, RECT *client) {
    GetClientRect(w, client);
    int cw = client->right, ch = client->bottom, dw = cw, dh = ch;
    if (g_scale == SCALE_FIT) {
        if (cw * GAME_H > ch * GAME_W) dw = ch * GAME_W / GAME_H; else dh = cw * GAME_H / GAME_W;
    } else if (g_scale == SCALE_INTEGER) {
        int k = min(cw / GAME_W, ch / GAME_H);
        if (k < 1) { if (cw * GAME_H > ch * GAME_W) dw = ch * GAME_W / GAME_H; else dh = cw * GAME_H / GAME_W; }
        else { dw = GAME_W * k; dh = GAME_H * k; }
    }
    RECT d = { (cw - dw) / 2, (ch - dh) / 2, (cw - dw) / 2 + dw, (ch - dh) / 2 + dh };
    return d;
}

/* ---- upscaler ----
   The game renders 1024x768 into the device's back buffer. On Present we copy it
   to a texture, run the shaders into our own swap chain of the window's size and
   present that one instead; the game's own state is saved and restored. */

typedef struct Blob Blob;
typedef struct {
    HRESULT(WINAPI *QueryInterface)(Blob *, REFIID, void **);
    ULONG(WINAPI *AddRef)(Blob *);
    ULONG(WINAPI *Release)(Blob *);
    void *(WINAPI *GetBufferPointer)(Blob *);
    SIZE_T(WINAPI *GetBufferSize)(Blob *);
} BlobVtbl;
struct Blob { const BlobVtbl *v; };
typedef HRESULT(WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, Blob **, Blob **);

static IDirect3DPixelShader9 *g_ps_catmull, *g_ps_rcas, *g_ps_sharp;
static IDirect3DTexture9 *g_tex_src, *g_tex_mid;
static IDirect3DSwapChain9 *g_chain;
static IDirect3DStateBlock9 *g_state;
static int g_mid_w, g_mid_h, g_chain_w, g_chain_h;
static int g_shaders_tried, g_pipeline_ok;

static IDirect3DPixelShader9 *compile_ps(IDirect3DDevice9 *dev, D3DCompile_t compile, const char *src, const char *name) {
    Blob *code = NULL, *err = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr = compile(src, strlen(src), name, NULL, NULL, "main", "ps_3_0", 0, 0, &code, &err);
    if (FAILED(hr)) {
        logf_("shader %s: compile failed %08lx %s", name, (unsigned long)hr, err ? (char *)err->v->GetBufferPointer(err) : "");
    } else if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)code->v->GetBufferPointer(code), &ps))) {
        logf_("shader %s: CreatePixelShader failed", name);
        ps = NULL;
    }
    if (code) code->v->Release(code);
    if (err) err->v->Release(err);
    return ps;
}

static void init_shaders(IDirect3DDevice9 *dev) {
    g_shaders_tried = 1;
    if (g_upscale == UPSCALE_BILINEAR) return;
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t compile = m ? (D3DCompile_t)GetProcAddress(m, "D3DCompile") : NULL;
    if (!compile) { logf_("d3dcompiler_47.dll not found: plain bilinear scaling"); return; }
    g_ps_catmull = compile_ps(dev, compile, k_ps_catmull, "catmull");
    g_ps_rcas = compile_ps(dev, compile, k_ps_rcas, "rcas");
    g_ps_sharp = compile_ps(dev, compile, k_ps_sharp, "sharp");
    g_pipeline_ok = g_upscale == UPSCALE_FSR ? (g_ps_catmull && g_ps_rcas) : g_ps_sharp != NULL;
    logf_("upscaler %s", g_pipeline_ok ? (g_upscale == UPSCALE_FSR ? "fsr (catmull-rom + rcas)" : "sharp") : "unavailable");
}

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* D3DPOOL_DEFAULT objects and the swap chain must go before Reset() */
static void release_targets(void) {
    RELEASE(g_tex_src);
    RELEASE(g_tex_mid);
    RELEASE(g_chain);
    RELEASE(g_state);
    g_mid_w = g_mid_h = g_chain_w = g_chain_h = 0;
}

static int ensure_targets(IDirect3DDevice9 *dev, int cw, int ch, int dw, int dh) {
    if (!g_tex_src && FAILED(IDirect3DDevice9_CreateTexture(dev, GAME_W, GAME_H, 1, D3DUSAGE_RENDERTARGET,
                                                           D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_tex_src, NULL)))
        return 0;
    if (g_upscale == UPSCALE_FSR && (g_mid_w != dw || g_mid_h != dh)) {
        RELEASE(g_tex_mid);
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, dw, dh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                  D3DPOOL_DEFAULT, &g_tex_mid, NULL)))
            return 0;
        g_mid_w = dw; g_mid_h = dh;
    }
    if (g_chain_w != cw || g_chain_h != ch) {
        RELEASE(g_chain);
        D3DPRESENT_PARAMETERS pp = { 0 };
        pp.BackBufferWidth = cw;
        pp.BackBufferHeight = ch;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = g_hwnd;
        pp.Windowed = TRUE;
        pp.PresentationInterval = g_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
        if (FAILED(IDirect3DDevice9_CreateAdditionalSwapChain(dev, &pp, &g_chain))) return 0;
        g_chain_w = cw; g_chain_h = ch;
        logf_("output %dx%d, image %dx%d", cw, ch, dw, dh);
    }
    if (!g_state && FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &g_state))) return 0;
    return 1;
}

typedef struct { float x, y, z, rhw, u, v; } quad_vtx;

static void draw_quad(IDirect3DDevice9 *dev, float x0, float y0, float x1, float y1) {
    /* -0.5: D3D9 maps texel centres to pixel centres this way */
    quad_vtx q[4] = { { x0 - 0.5f, y0 - 0.5f, 0, 1, 0, 0 }, { x1 - 0.5f, y0 - 0.5f, 0, 1, 1, 0 },
                      { x0 - 0.5f, y1 - 0.5f, 0, 1, 0, 1 }, { x1 - 0.5f, y1 - 0.5f, 0, 1, 1, 1 } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

static void set_sampler(IDirect3DDevice9 *dev, int linear) {
    DWORD f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, f);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, f);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
}

/* returns S_FALSE when the pipeline cannot run and the plain path must be used */
/* ---- screenshots: F12 saves the game picture (1024x768) to build\screenshots\*.bmp ----
   Windows' own capture (Win+Shift+S) does not get the game's frame. */
static char g_shot_dir[MAX_PATH];

static void save_screenshot(IDirect3DDevice9 *dev, IDirect3DSurface9 *rt) {
    IDirect3DSurface9 *sys = NULL;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, GAME_W, GAME_H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) return;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        CreateDirectoryA(g_shot_dir, NULL);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\rip3_%04d%02d%02d_%02d%02d%02d_%03d.bmp", g_shot_dir, t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        FILE *f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER fh = { 0x4D42 };
            BITMAPINFOHEADER ih = { sizeof ih, GAME_W, -GAME_H, 1, 32, BI_RGB };
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + GAME_W * GAME_H * 4;
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            for (int y = 0; y < GAME_H; y++) fwrite((uint8_t *)lr.pBits + y * lr.Pitch, 4, GAME_W, f);
            fclose(f);
            logf_("screenshot %s", path);
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
}

static HRESULT upscale_present(IDirect3DDevice9 *dev, HWND w) {
    HRESULT hr = IDirect3DDevice9_TestCooperativeLevel(dev);
    if (hr != D3D_OK) return hr; /* lost: let the game see it and Reset() */
    RECT client, d = dest_rect(w, &client);
    int dw = d.right - d.left, dh = d.bottom - d.top;
    if (client.right <= 0 || client.bottom <= 0 || dw <= 0 || dh <= 0) return S_FALSE;
    if (!ensure_targets(dev, client.right, client.bottom, dw, dh)) { logf_("upscaler: target creation failed"); g_pipeline_ok = 0; return S_FALSE; }

    IDirect3DSurface9 *bb = NULL, *src = NULL, *mid = NULL, *out = NULL, *old_rt = NULL, *old_ds = NULL;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DTexture9_GetSurfaceLevel(g_tex_src, 0, &src);
    IDirect3DDevice9_StretchRect(dev, bb, NULL, src, NULL, D3DTEXF_NONE);
    static SHORT f12;
    SHORT k = GetAsyncKeyState(VK_F12);
    if ((k & 0x8000) && !(f12 & 0x8000) && GetForegroundWindow() == w) save_screenshot(dev, src);
    f12 = k;

    IDirect3DStateBlock9_Capture(g_state);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &old_rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &old_ds);
    IDirect3DSwapChain9_GetBackBuffer(g_chain, 0, D3DBACKBUFFER_TYPE_MONO, &out);

    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetTexture(dev, 1, NULL);

    IDirect3DDevice9_BeginScene(dev);
    if (g_upscale == UPSCALE_FSR) {
        /* pass 1: Catmull-Rom 1024x768 -> dw x dh */
        IDirect3DTexture9_GetSurfaceLevel(g_tex_mid, 0, &mid);
        IDirect3DDevice9_SetRenderTarget(dev, 0, mid);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_tex_src);
        set_sampler(dev, 1);
        float c0[4] = { GAME_W, GAME_H, 1.0f / GAME_W, 1.0f / GAME_H };
        IDirect3DDevice9_SetPixelShader(dev, g_ps_catmull);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
        draw_quad(dev, 0, 0, (float)dw, (float)dh);
        /* pass 2: RCAS into the window-sized back buffer */
        IDirect3DDevice9_SetRenderTarget(dev, 0, out);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_tex_mid);
        set_sampler(dev, 0);
        float c1[4] = { 1.0f / dw, 1.0f / dh, exp2f(-g_sharpness), 0 };
        IDirect3DDevice9_SetPixelShader(dev, g_ps_rcas);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c1, 1);
        draw_quad(dev, (float)d.left, (float)d.top, (float)d.right, (float)d.bottom);
    } else {
        IDirect3DDevice9_SetRenderTarget(dev, 0, out);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_tex_src);
        set_sampler(dev, 1);
        float c0[4] = { GAME_W, GAME_H, 1.0f / GAME_W, 1.0f / GAME_H };
        float c1[4] = { (float)dh / GAME_H, 0, 0, 0 };
        IDirect3DDevice9_SetPixelShader(dev, g_ps_sharp);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, c1, 1);
        draw_quad(dev, (float)d.left, (float)d.top, (float)d.right, (float)d.bottom);
    }
    IDirect3DDevice9_EndScene(dev);

    IDirect3DDevice9_SetRenderTarget(dev, 0, old_rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, old_ds);
    IDirect3DStateBlock9_Apply(g_state);

    RELEASE(bb); RELEASE(src); RELEASE(mid); RELEASE(out); RELEASE(old_rt); RELEASE(old_ds);
    return IDirect3DSwapChain9_Present(g_chain, NULL, NULL, w, NULL, 0);
}

static HRESULT WINAPI my_Present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty) {
    HWND w = wnd ? wnd : g_hwnd;
    if (!g_shaders_tried) init_shaders(dev);
    if (g_pipeline_ok && w && !src && !dst) {
        if (g_fps_cap > 0) *(volatile int *)ADDR_FPS_CAP = g_fps_cap;
        HRESULT hr = upscale_present(dev, w);
        if (hr != S_FALSE) return hr;
    }
    if (g_fps_cap > 0) {
        static int logged;
        if (!logged++) logf_("game fps cap was %d, set to %d", *(volatile int *)ADDR_FPS_CAP, g_fps_cap);
        *(volatile int *)ADDR_FPS_CAP = g_fps_cap;
    }
    if (!w || dst) return real_Present(dev, src, dst, wnd, dirty);
    RECT client, d = dest_rect(w, &client);
    HRESULT hr = real_Present(dev, src, &d, wnd, dirty);
    /* black bars around the frame */
    if (d.left > 0 || d.top > 0) {
        HDC dc = GetDC(w);
        HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bars[4] = { { 0, 0, client.right, d.top }, { 0, d.bottom, client.right, client.bottom },
                         { 0, d.top, d.left, d.bottom }, { d.right, d.top, client.right, d.bottom } };
        for (int i = 0; i < 4; i++) if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top) FillRect(dc, &bars[i], b);
        ReleaseDC(w, dc);
    }
    return hr;
}

static HRESULT WINAPI my_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
    release_targets();
    fix_params(pp);
    return real_Reset(dev, pp);
}

static HRESULT WINAPI my_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    /* the game keeps this struct and passes it to Reset() later, so the fix sticks */
    fix_params(pp);
    if (!pp->hDeviceWindow) pp->hDeviceWindow = focus;
    HRESULT hr = real_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    logf_("CreateDevice windowed %ux%u vsync=%d -> %08lx", pp->BackBufferWidth, pp->BackBufferHeight, g_vsync, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        void *o;
        if ((o = hook_vtable(*out, 16, my_Reset))) real_Reset = (Reset_t)o;
        if ((o = hook_vtable(*out, 17, my_Present))) real_Present = (Present_t)o;
    }
    return hr;
}

static IDirect3D9 *WINAPI my_Direct3DCreate9(UINT sdk) {
    IDirect3D9 *d3d = real_Direct3DCreate9(sdk);
    if (d3d) {
        void *o = hook_vtable(d3d, 16, my_CreateDevice);
        if (o) real_CreateDevice = (CreateDevice_t)o;
    }
    logf_("Direct3DCreate9 -> %p", (void *)d3d);
    return d3d;
}

/* ---- settings ---- */

static void load_settings(const char *dir) {
    char ini[MAX_PATH], v[32];
    snprintf(ini, sizeof ini, "%s\\rip3video.ini", dir);
    GetPrivateProfileStringA("Video", "Mode", "borderless", v, sizeof v, ini);
    g_mode = _stricmp(v, "window") == 0 ? MODE_WINDOW : MODE_BORDERLESS;
    GetPrivateProfileStringA("Video", "Scale", "fit", v, sizeof v, ini);
    g_scale = _stricmp(v, "stretch") == 0 ? SCALE_STRETCH : _stricmp(v, "integer") == 0 ? SCALE_INTEGER : SCALE_FIT;
    /* off by default: loading screens present after every texture and would wait a
       vblank each time; windowed output is composed by DWM, so it does not tear */
    g_vsync = GetPrivateProfileIntA("Video", "VSync", 0, ini);
    g_win_scale = GetPrivateProfileIntA("Video", "WindowScale", 1, ini);
    if (g_win_scale < 1) g_win_scale = 1;
    g_fps_cap = GetPrivateProfileIntA("Video", "FpsLimit", 0, ini);
    g_skip_logos = GetPrivateProfileIntA("Video", "SkipLogos", 1, ini);
    g_profile = GetPrivateProfileIntA("Video", "Profile", 0, ini);
    g_fast_zip = GetPrivateProfileIntA("Video", "FastLoad", 1, ini);
    GetPrivateProfileStringA("Video", "Upscale", "fsr", v, sizeof v, ini);
    g_upscale = _stricmp(v, "bilinear") == 0 ? UPSCALE_BILINEAR : _stricmp(v, "sharp") == 0 ? UPSCALE_SHARP : UPSCALE_FSR;
    GetPrivateProfileStringA("Video", "Sharpness", "0.2", v, sizeof v, ini);
    g_sharpness = (float)atof(v);
    if (g_sharpness < 0) g_sharpness = 0;
    if (g_sharpness > 2) g_sharpness = 2;
}

/* ---- install ---- */

/* ---- startup profiler (Profile=1 in rip3video.ini) ----
   Samples the main thread 200 times a second for 25 s after the window is made and logs
   where the time goes: by module, and for the game's code by the nearest game address on
   the stack (EBP chain), so time in DLLs is charged to the game function that called them. */
typedef struct { uint32_t key; uint32_t hits; } prof_bucket;
static prof_bucket g_prof[4096];

static void prof_add(uint32_t key) {
    uint32_t h = (key * 2654435761u) >> 20;
    for (int i = 0; i < 4096; i++) {
        prof_bucket *b = &g_prof[(h + i) & 4095];
        if (b->key == key) { b->hits++; return; }
        if (!b->key) { b->key = key; b->hits = 1; return; }
    }
}

static int prof_cmp(const void *a, const void *b) { return (int)((const prof_bucket *)b)->hits - (int)((const prof_bucket *)a)->hits; }

static int in_game_code(uint32_t a) { return a >= 0x00401000u && a < 0x004B8000u; }

static DWORD WINAPI profiler(LPVOID p) {
    (void)p;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
    if (!th) { logf_("profile: OpenThread failed %lu", GetLastError()); return 0; }
    logf_("profile: started");
    DWORD t0 = GetTickCount();
    uint32_t total = 0, in_exe = 0;
    timeBeginPeriod(1);
    int in_loop = 0;
    /* until the main loop runs (the frame timer FUN_0043efb0 seen 40 samples in a row) */
    while (GetTickCount() - t0 < 30000 && in_loop < 40) {
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(th) != (DWORD)-1) {
            if (GetThreadContext(th, &c)) {
                uint32_t eip = c.Eip, key = eip & ~0x3Fu;
                if (in_game_code(eip)) in_exe++;
                else {
                    /* charge to the first game address up the EBP chain */
                    uint32_t ebp = c.Ebp, found = 0;
                    for (int d = 0; d < 32 && ebp; d++) {
                        MEMORY_BASIC_INFORMATION mbi;
                        if (!VirtualQuery((void *)ebp, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                        uint32_t ret = ((uint32_t *)ebp)[1];
                        if (in_game_code(ret)) { found = ret; break; }
                        uint32_t next = ((uint32_t *)ebp)[0];
                        if (next <= ebp) break;
                        ebp = next;
                    }
                    key = found ? (found & ~0x3Fu) | 1 : 0xFFFFFFF1u;
                }
                /* the frame timer mostly sleeps, so look at the charged address, not EIP */
                /* menu: the frame timer takes most of each frame, drawing the rest */
                uint32_t at = key & ~1u;
                static uint8_t win[100];
                static int wi, wsum;
                wsum -= win[wi];
                win[wi] = at >= 0x0043EF80u && at < 0x0043F0C0u;
                wsum += win[wi];
                wi = (wi + 1) % 100;
                if (total >= 100 && wsum >= 40) in_loop = 40;
                prof_add(key);
                total++;
            }
            ResumeThread(th);
        }
        Sleep(5);
    }
    timeEndPeriod(1);
    qsort(g_prof, 4096, sizeof g_prof[0], prof_cmp);
    logf_("profile: loading took %lu ms after the window", GetTickCount() - t0);
    logf_("profile: %u samples, %u in game code (rest charged to the calling game address, key|1)", total, in_exe);
    for (int i = 0; i < 40 && g_prof[i].hits; i++)
        logf_("  %08x %s %5u  %.1f%%", g_prof[i].key & ~1u, (g_prof[i].key & 1) ? "via-dll" : "exe    ", g_prof[i].hits,
              100.0 * g_prof[i].hits / total);
    CloseHandle(th);
    return 0;
}

static DWORD WINAPI worker(LPVOID self) {
    char dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA((HMODULE)self, dir, sizeof dir);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\rip3video.log", dir);
    g_log = fopen(path, "w");
    snprintf(g_shot_dir, sizeof g_shot_dir, "%s\\screenshots", dir);
    load_settings(dir);

    real_Direct3DCreate9 = (Direct3DCreate9_t)GetProcAddress(LoadLibraryA("d3d9.dll"), "Direct3DCreate9");
    real_CreateWindowExA = (CreateWindowExA_t)GetProcAddress(GetModuleHandleA("user32.dll"), "CreateWindowExA");

    /* wait for the packer to fill the import table, then swap the entries */
    DWORD t0 = GetTickCount();
    while (*(void **)k_slots_d3dcreate[0] != (void *)real_Direct3DCreate9) {
        if (GetTickCount() - t0 > 20000) { logf_("import table not filled in time, nothing changed"); return 0; }
        if (g_hwnd || FindWindowA(NULL, WINDOW_TITLE)) { logf_("injected too late: window exists"); return 0; }
        Sleep(0);
    }
    for (int i = 0; i < 2; i++) {
        if (*(void **)k_slots_createwnd[i] == (void *)real_CreateWindowExA) write_ptr((void *)k_slots_createwnd[i], my_CreateWindowExA);
        if (*(void **)k_slots_d3dcreate[i] == (void *)real_Direct3DCreate9) write_ptr((void *)k_slots_d3dcreate[i], my_Direct3DCreate9);
    }
    logf_("imports hooked after %lu ms (mode=%d scale=%d vsync=%d)", GetTickCount() - t0, g_mode, g_scale, g_vsync);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        /* crisp output on 125%/150% Windows scaling; must happen before the window exists */
        SetProcessDPIAware();
        CloseHandle(CreateThread(NULL, 0, worker, h, 0, NULL));
    }
    return TRUE;
}
