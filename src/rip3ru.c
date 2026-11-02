// rip3ru.dll - Russian language for RIP 3 (exe SHA-256 C4305AA6...D925D2).
// Only game DATA in memory is changed, never code: PECompact checks the code
// after unpacking ("Application corrupt").
//   1. DAT_004fc60c (language) = 1. The Steam build forces 0 at the start of
//      FUN_004138a0; menus pick the Russian table when it is 1.
//   2. The developers' Russian string table (0x4F1490, 83 entries) is copied over
//      the English one (0x4F1340), so code that reads the English table directly
//      also shows Russian.
//   3. perks.dat texts (messages, story, perks, heroes, credits): the parsed table
//      (key[32] + char* text, stride 0x24) gets pointers to our translations.
//   4. levels.dat names: written into the 100 + 10 name buffers.
// Translations: build\lang\perks.ru.txt and levels.ru.txt, CP1251.
// Must be injected right after launch: inject.exe --wait build\rip3ru.dll
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ADDR_LANG        0x004FC60Cu
#define ADDR_LANG_STORE  0x004138DCu /* push ebx; mov [DAT_004fc60c], bl */
#define ADDR_TABLE_EN    0x004F1340u
#define ADDR_TABLE_RU    0x004F1490u
#define TABLE_ENTRIES    83
#define ADDR_MULTIPLAYER_RU 0x004BC6CCu /* "СЕТЕВАЯ ИГРА", pointed to by the RU table */
#define ADDR_GAME        0x00503B50u
#define GAME_PERKS_OBJ   0x1114u     /* -> object loaded by FUN_004400d0 (ecx at 0x413F87) */
#define PERKS_KEY        0x111Cu
#define PERKS_TEXT       0x113Cu
#define PERKS_STRIDE     0x24u
#define PERKS_COUNT      0x34F8u
#define PERKS_MAX        0x100u
#define GAME_LEVEL_NAMES 0x0DB0u     /* char*[100], "Level Names" L1..L100 */
#define GAME_MLEVEL_NAMES 0x0F40u    /* char*[10],  ML1..ML10 */
#define LEVEL_NAME_SIZE  100
#define RIP3_LANG_RU     1

static const uint8_t k_store[] = { 0x53, 0x88, 0x1D, 0x0C, 0xC6, 0x4F, 0x00 };

static FILE *g_log;
static char g_dir[MAX_PATH];

static int language_is_ru(void) {
    char path[MAX_PATH], value[16];
    snprintf(path, sizeof path, "%s\\rip3mod.ini", g_dir);
    GetPrivateProfileStringA("General", "Language", "ru", value, sizeof value, path);
    return _stricmp(value, "en") != 0 && _stricmp(value, "english") != 0;
}

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static char *load_file(const char *name, size_t *len) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\lang\\%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { logf_("missing %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f);
    fclose(f);
    buf[n] = 0;
    *len = n;
    return buf;
}

/* ---- 2. string table ---- */

static void apply_table(void) {
    DWORD old;
    VirtualProtect((void *)ADDR_TABLE_EN, TABLE_ENTRIES * 4, PAGE_READWRITE, &old);
    memcpy((void *)ADDR_TABLE_EN, (void *)ADDR_TABLE_RU, TABLE_ENTRIES * 4);
    VirtualProtect((void *)ADDR_TABLE_EN, TABLE_ENTRIES * 4, old, &old);
}

/* The old LAN deathmatch button says "СЕТЕВАЯ ИГРА"; our screen is co-op.
   The Russian table points at this fixed string, so rewrite it in place. */
static void rename_multiplayer(void) {
    static const char old_ru[] = {
        (char)0xD1, (char)0xC5, (char)0xD2, (char)0xC5, (char)0xC2, (char)0xC0,
        (char)0xDF, ' ', (char)0xC8, (char)0xC3, (char)0xD0, (char)0xC0
    };
    static const char new_ru[] = {
        (char)0xCA, (char)0xCE, (char)0xCE, (char)0xCF, (char)0xC5,
        (char)0xD0, (char)0xC0, (char)0xD2, (char)0xC8, (char)0xC2, 0
    };
    char *p = (char *)ADDR_MULTIPLAYER_RU;
    if (memcmp(p, old_ru, sizeof old_ru) != 0) {
        logf_("menu: MULTIPLAYER label not found");
        return;
    }
    DWORD old;
    if (!VirtualProtect(p, 13, PAGE_READWRITE, &old)) {
        logf_("menu: cannot unlock MULTIPLAYER label (%lu)", GetLastError());
        return;
    }
    memcpy(p, new_ru, sizeof new_ru);
    memset(p + sizeof new_ru, 0, 13 - sizeof new_ru);
    VirtualProtect(p, 13, old, &old);
    logf_("menu: MULTIPLAYER -> КООПЕРАТИВ");
}

/* ---- 3. perks.dat ---- */

typedef struct { char key[32]; char *text; } entry_t;
static entry_t g_perks[PERKS_MAX];
static int g_nperks;

static void parse_perks(char *buf) {
    /* "#key\n" followed by text up to the next line starting with '#' */
    char *p = buf;
    while ((p = strchr(p, '#')) && g_nperks < PERKS_MAX) {
        if (p != buf && p[-1] != '\n') { p++; continue; }
        char *eol = strchr(p, '\n');
        if (!eol) break;
        entry_t *e = &g_perks[g_nperks];
        size_t kl = eol - p - 1;
        while (kl && (p[kl] == '\r' || p[kl] == ' ')) kl--;
        if (kl >= sizeof e->key) kl = sizeof e->key - 1;
        memcpy(e->key, p + 1, kl);
        e->key[kl] = 0;
        char *t = eol + 1, *end = t;
        while (*end && !(*end == '#' && end[-1] == '\n')) end++;
        size_t tl = end - t;
        while (tl && (t[tl - 1] == '\n' || t[tl - 1] == '\r')) tl--;
        e->text = malloc(tl + 1);
        size_t o = 0;
        for (size_t i = 0; i < tl; i++) if (t[i] != '\r') e->text[o++] = t[i];
        e->text[o] = 0;
        g_nperks++;
        p = end;
    }
}

/* The game frees these texts with its own CRT (_free), so they must come from
   its own heap: our DLL's malloc uses a different heap -> heap corruption in ntdll. */
#define ADDR_GAME_MALLOC 0x004A2D8Cu
typedef void *(__cdecl *game_malloc_fn)(size_t);

static char *game_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = ((game_malloc_fn)ADDR_GAME_MALLOC)(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int apply_perks(void) {
    uint8_t *obj = *(uint8_t **)(ADDR_GAME + GAME_PERKS_OBJ);
    if (!readable(obj, PERKS_COUNT + 4)) return 0;
    uint32_t n = *(uint32_t *)(obj + PERKS_COUNT);
    if (n == 0 || n > PERKS_MAX) return 0;
    int done = 0;
    for (uint32_t i = 0; i < n; i++) {
        char *key = (char *)(obj + PERKS_KEY + i * PERKS_STRIDE);
        char **text = (char **)(obj + PERKS_TEXT + i * PERKS_STRIDE);
        for (int k = 0; k < g_nperks; k++) {
            if (strcmp(key, g_perks[k].key) != 0) continue;
            char *t = game_strdup(g_perks[k].text);
            if (t) { *text = t; done++; } /* the English text is left allocated: freeing it here races the game */
            break;
        }
    }
    logf_("perks: %d of %u texts replaced", done, n);
    return 1;
}

/* ---- 4. level names ---- */

static int apply_levels(char *buf) {
    char **names = (char **)(ADDR_GAME + GAME_LEVEL_NAMES);
    char **mnames = (char **)(ADDR_GAME + GAME_MLEVEL_NAMES);
    if (!readable(names[0], LEVEL_NAME_SIZE) || names[0][0] == 0) return 0;
    int done = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        int idx;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char **dst = NULL;
        if (sscanf(line, "ML%d=", &idx) == 1 && idx >= 1 && idx <= 10) dst = &mnames[idx - 1];
        else if (sscanf(line, "L%d=", &idx) == 1 && idx >= 1 && idx <= 100) dst = &names[idx - 1];
        if (!dst || !readable(*dst, LEVEL_NAME_SIZE)) continue;
        strncpy(*dst, eq + 1, LEVEL_NAME_SIZE - 1);
        (*dst)[LEVEL_NAME_SIZE - 1] = 0;
        done++;
    }
    logf_("levels: %d names replaced", done);
    return 1;
}

/* ---- 6. weapon names ----
   FUN_00456910 copies "name=" from weapons.dat / lweapons.dat into a static array
   (72 records of 0x164 bytes, name buffer 0x100 at the start) on every level load,
   so the names are re-checked periodically and replaced when they are English. */

#define ADDR_WEAPON_NAMES 0x004FD498u
#define WEAPON_STRIDE     0x164u
#define WEAPON_COUNT      72

typedef struct { char en[64], ru[64]; } weapon_name_t;
static weapon_name_t g_weapons[64];
static int g_nweapons;

static void parse_weapons(char *buf) {
    for (char *line = strtok(buf, "\r\n"); line && g_nweapons < 64; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        strncpy(g_weapons[g_nweapons].en, line, 63);
        strncpy(g_weapons[g_nweapons].ru, eq + 1, 63);
        g_nweapons++;
    }
}

static int apply_weapons(void) {
    int n = 0;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        char *name = (char *)(ADDR_WEAPON_NAMES + i * WEAPON_STRIDE);
        if (!name[0]) continue;
        for (int k = 0; k < g_nweapons; k++) {
            if (strcmp(name, g_weapons[k].en) != 0) continue;
            strcpy(name, g_weapons[k].ru);
            n++;
            break;
        }
    }
    return n;
}

/* ---- 5. pictures with baked-in English text ----
   data.pak is a zip read through FUN_00444340 (locate, name is arg 2) and
   FUN_00446510 (read into a buffer of the original size). Calls to both are
   redirected; when the last located name has a file in build\img\, that file
   (same size, JPEG padded with zeros) replaces the buffer contents.
   Patching code is only safe after the PECompact check, i.e. once the window exists. */

#define ADDR_PAK_LOCATE  0x00444340u
#define ADDR_PAK_READ    0x00446510u
#define CODE_START       0x00401000u
#define CODE_END         0x004B0000u

typedef int(__cdecl *pak_locate_fn)(void *zip, const char *name, int cs, void *pos, void *info);
typedef int(__cdecl *pak_read_fn)(void *zip, void *pos, void *buf, unsigned size);
static char g_last_name[MAX_PATH];
static int g_replaced;

static int __cdecl my_locate(void *zip, const char *name, int cs, void *pos, void *info) {
    int r = ((pak_locate_fn)ADDR_PAK_LOCATE)(zip, name, cs, pos, info);
    if (r == 0 && name) { strncpy(g_last_name, name, sizeof g_last_name - 1); }
    else g_last_name[0] = 0;
    return r;
}

/* names (as in data.pak, '/' separators) that have a replacement in build\img */
static char g_img_names[256][MAX_PATH];
static int g_nimg;

static void scan_images(const char *dir, const char *prefix) {
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char sub[MAX_PATH], name[MAX_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        snprintf(name, sizeof name, "%s%s%s", prefix, *prefix ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan_images(sub, name);
        else if (g_nimg < 256) strcpy(g_img_names[g_nimg++], name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int has_image(const char *name) {
    for (int i = 0; i < g_nimg; i++) if (_stricmp(g_img_names[i], name) == 0) return 1;
    return 0;
}

static int __cdecl my_read(void *zip, void *pos, void *buf, unsigned size) {
    int r = ((pak_read_fn)ADDR_PAK_READ)(zip, pos, buf, size);
    if (r != 0 || !g_last_name[0] || !buf) return r;
    if (!has_image(g_last_name)) { g_last_name[0] = 0; return r; }
    char path[MAX_PATH];
    int n = snprintf(path, sizeof path, "%s\\img\\%s", g_dir, g_last_name);
    for (int i = n - (int)strlen(g_last_name); i < n; i++) if (path[i] == '/') path[i] = '\\';
    FILE *f = fopen(path, "rb");
    if (!f) return r;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && (unsigned)len <= size) {
        fread(buf, 1, len, f);
        g_replaced++;
        logf_("picture: %s", g_last_name);
    } else {
        logf_("picture: %s skipped (%ld > %u bytes)", g_last_name, len, size);
    }
    fclose(f);
    g_last_name[0] = 0;
    return r;
}

static int redirect_calls(uint32_t target, void *to) {
    int n = 0;
    for (uint32_t p = CODE_START; p + 5 <= CODE_END; p++) {
        if (*(uint8_t *)p != 0xE8) continue;
        if (p + 5 + *(int32_t *)(p + 1) != target) continue;
        int32_t rel = (int32_t)((uint32_t)to - (p + 5));
        DWORD old;
        VirtualProtect((void *)(p + 1), 4, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t *)(p + 1) = rel;
        VirtualProtect((void *)(p + 1), 4, old, &old);
        n++;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    return n;
}

/* ---- main ---- */

static BOOL CALLBACK find_own_window(HWND w, LPARAM out) {
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    /* any window will do: it exists before it is shown and before the first loading screen */
    if (pid != GetCurrentProcessId()) return TRUE;
    *(HWND *)out = w;
    return FALSE;
}

static DWORD WINAPI worker(LPVOID self) {
    char path[MAX_PATH];
    GetModuleFileNameA((HMODULE)self, g_dir, sizeof g_dir);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\rip3ru.log", g_dir);
    g_log = fopen(path, "a");
    if (!language_is_ru()) {
        logf_("game language: English (Russian translation disabled)");
        return 0;
    }

    size_t len;
    char *perks = load_file("perks.ru.txt", &len);
    char *levels = load_file("levels.ru.txt", &len);
    if (perks) parse_perks(perks);
    char *weapons = load_file("weapons.ru.txt", &len);
    if (weapons) parse_weapons(weapons);
    snprintf(path, sizeof path, "%s\\img", g_dir);
    scan_images(path, "");
    logf_("loaded %d perk texts, %d pictures", g_nperks, g_nimg);

    /* 1. wait for the window: the packer is done and the language reset has run */
    DWORD t0 = GetTickCount();
    HWND w = NULL;
    while (!w) {
        if (GetTickCount() - t0 > 60000) { logf_("game window not found, nothing changed"); return 0; }
        Sleep(1);
        EnumWindows(find_own_window, (LPARAM)&w);
    }
    if (memcmp((void *)ADDR_LANG_STORE, k_store, sizeof k_store) != 0) {
        logf_("unknown build, nothing changed");
        return 0;
    }
    *(volatile uint8_t *)ADDR_LANG = RIP3_LANG_RU;
    apply_table();
    rename_multiplayer();
    int nl = redirect_calls(ADDR_PAK_LOCATE, my_locate);
    int nr = redirect_calls(ADDR_PAK_READ, my_read);
    logf_("language + string table + pak hooks (%d/%d) after %lu ms", nl, nr, GetTickCount() - t0);

    /* perks.dat and level names are loaded later in FUN_004138a0 */
    int perks_ok = !perks, levels_ok = !levels;
    while ((!perks_ok || !levels_ok) && GetTickCount() - t0 < 120000) {
        if (!perks_ok) perks_ok = apply_perks();
        if (!levels_ok) levels_ok = apply_levels(levels);
        Sleep(20);
    }
    if (!perks_ok || !levels_ok) logf_("timeout: perks=%d levels=%d", perks_ok, levels_ok);

    /* weapons are reloaded with every level: keep watching for the whole session */
    for (int total = 0;; Sleep(300)) {
        int n = apply_weapons();
        if (n) { total += n; logf_("weapons: %d names replaced (total %d)", n, total); }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(NULL, 0, worker, h, 0, NULL));
    }
    return TRUE;
}
