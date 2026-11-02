// rip3coop.exe - online co-op launcher for RIP 3. Holds the internet UDP socket, finds the
// public address with STUN, shows an invite code, punches through NAT to the friend and
// forwards game packets between the friend and rip3net.dll (127.0.0.1, see netproto.h).
// Also has two test bots that stand in for the friend:
//   bot-friend: player 2 with scripted keys, for testing the host side
//   bot-host:   replays local\runs\try2 (record/replay probe) to the game as the host and
//               compares the game's per-frame checks with hash_record.txt
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "netproto.h"

#define GAME_DIR "C:\\Program Files (x86)\\Steam\\steamapps\\common\\RIP 3 - The Last Hero\\RIP3"
#define REC_DIR  "..\\local\\runs\\try2"
#define REC_SEED 0x1234567u

static SOCKET g_inet = INVALID_SOCKET, g_link = INVALID_SOCKET;
static struct sockaddr_in g_dll;            /* 127.0.0.1:NP_PORT_DLL */
static struct sockaddr_in g_peer_pub, g_peer_lan, g_peer;
static int g_connected, g_role;
static int g_radmin;                        /* direct mode: code carries the 26.x VPN address */
static uint16_t g_nonce, g_peer_nonce, g_rtt;
static DWORD g_last_peer;

/* ---- helpers ---- */

/* service mode (started hidden by the game): the game's overlay shows the last line */
static int g_service;
static char g_note[160];
static void send_info(void);
static void service_tick(void);
static void handle_cmd(const uint8_t *p, int n);

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_note, sizeof g_note, fmt, ap);
    va_end(ap);
    if (g_service) { send_info(); return; }
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, g_note);
    fflush(stdout);
}

static void read_line(char *buf, int n) {
    if (!fgets(buf, n, stdin)) buf[0] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
}

static void (*g_idle_hook)(void);

static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; /* Crockford */

/* Invite code: the public IPv4, plus the public port only when the router did not keep our
   fixed local port (NP_PORT_GAME). 9 characters (45 bits: ip 32, flag 1, check 12) or
   13 characters (+ port 16). */
#define NP_PORT_GAME 27605
#define CODE_CHECK(ip, port) ((uint32_t)((((ip) ^ ((uint32_t)(port) * 40503u)) * 2654435761u) >> 20) & 0xFFF)

static void encode_code(uint32_t ip_host, uint16_t port, char *out) {
    int has_port = port != NP_PORT_GAME;
    uint64_t v = (uint64_t)ip_host | ((uint64_t)has_port << 32) | ((uint64_t)CODE_CHECK(ip_host, port) << 33);
    if (has_port) v |= (uint64_t)port << 45;
    int chars = has_port ? 13 : 9, w = 0;
    for (int i = chars - 1; i >= 0; i--) {
        out[w++] = B32[(v >> (i * 5)) & 31];
        if (i == 4) out[w++] = '-';
    }
    out[w] = 0;
}

static int decode_code(const char *s, uint32_t *ip_host, uint16_t *port) {
    uint64_t v = 0;
    int chars = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '-' || c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        const char *p = strchr(B32, c);
        if (!p || chars >= 13) return 0;
        v = (v << 5) | (uint64_t)(p - B32);
        chars++;
    }
    if (chars != 9 && chars != 13) return 0;
    int has_port = (v >> 32) & 1;
    if (has_port != (chars == 13)) return 0;
    *ip_host = (uint32_t)v;
    *port = has_port ? (uint16_t)(v >> 45) : NP_PORT_GAME;
    return ((v >> 33) & 0xFFF) == CODE_CHECK(*ip_host, *port);
}

static void copy_to_clipboard(const char *s) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t n = strlen(s) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) { memcpy(GlobalLock(h), s, n); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
}


static SOCKET udp_bind(uint32_t ip, uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

/* ---- STUN (RFC 5389 binding request) ---- */

static int stun_query(const char *host, struct sockaddr_in *mapped) {
    struct addrinfo hints = { 0 }, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, "19302", &hints, &res) != 0) return 0;
    struct sockaddr_in srv = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
    for (int i = 8; i < 20; i++) req[i] = (uint8_t)rand();
    for (int attempt = 0; attempt < 4; attempt++) {
        sendto(g_inet, (char *)req, sizeof req, 0, (struct sockaddr *)&srv, sizeof srv);
        for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 500; Sleep(10)) {
            if (g_idle_hook) g_idle_hook(); /* the game must keep hearing from us meanwhile */
            uint8_t buf[512];
            struct sockaddr_in from;
            int fl = sizeof from;
            int n = recvfrom(g_inet, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n < 20 || buf[0] != 0x01 || buf[1] != 0x01 || memcmp(buf + 8, req + 8, 12) != 0) continue;
            for (int p = 20; p + 4 <= n;) {
                int type = (buf[p] << 8) | buf[p + 1], len = (buf[p + 2] << 8) | buf[p + 3];
                const uint8_t *v = buf + p + 4;
                if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 1) {
                    uint16_t port = (uint16_t)((v[2] << 8) | v[3]);
                    uint32_t ip;
                    memcpy(&ip, v + 4, 4);
                    if (type == 0x0020) { port ^= 0x2112; ip ^= htonl(0x2112A442); }
                    mapped->sin_family = AF_INET;
                    mapped->sin_port = htons(port);
                    mapped->sin_addr.s_addr = ip;
                    return 1;
                }
                p += 4 + ((len + 3) & ~3);
            }
        }
    }
    return 0;
}

/* LAN address of the interface that routes to the internet (no packet is sent) */
static uint32_t lan_ip(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    connect(s, (struct sockaddr *)&a, sizeof a);
    int al = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &al);
    closesocket(s);
    return a.sin_addr.s_addr;
}

/* ---- local link to the game ---- */

static void to_dll(const void *p, int n) { sendto(g_link, (const char *)p, n, 0, (struct sockaddr *)&g_dll, sizeof g_dll); }

static void send_link(void) {
    np_link l = { PK_LINK, (uint8_t)g_role, (uint8_t)g_connected, g_rtt };
    to_dll(&l, sizeof l);
}

static void print_status(const uint8_t *p, int n) {
    char text[sizeof(((np_status *)0)->text) + 1];
    int len = n - 1 < (int)sizeof text - 1 ? n - 1 : (int)sizeof text - 1;
    memcpy(text, p + 1, len);
    text[len] = 0;
    say("Игра: %s", text);
}

/* ---- bots: a simulated network with latency and loss towards the game ---- */

typedef struct { DWORD at; int n; uint8_t data[NP_MAX_PACKET]; } delayed;
static delayed g_q[512];
static int g_qn;
static int g_sim_latency = 35, g_sim_jitter = 15, g_sim_loss = 2; /* ms, ms, % */

static void bot_send(const void *p, int n) {
    if (rand() % 100 < g_sim_loss || g_qn == 512 || n > NP_MAX_PACKET) return;
    delayed *d = &g_q[g_qn++];
    d->at = GetTickCount() + g_sim_latency + rand() % (g_sim_jitter + 1);
    d->n = n;
    memcpy(d->data, p, n);
}

static void bot_flush(void) {
    DWORD now = GetTickCount();
    for (int i = 0; i < g_qn;) {
        if ((int)(now - g_q[i].at) >= 0) { to_dll(g_q[i].data, g_q[i].n); g_q[i] = g_q[--g_qn]; }
        else i++;
    }
}

static np_frame g_bot_local[NP_RING], g_bot_remote[NP_RING];
static uint32_t g_bot_have_local, g_bot_have_remote, g_bot_peer_ack, g_bot_session;
static np_start g_bot_start;
static uint8_t g_zero[INPUT_OBJ_SIZE];

static void bot_send_input(const uint8_t *base, uint32_t len) {
    uint8_t pkt[NP_MAX_PACKET];
    uint32_t first = g_bot_peer_ack;
    if (g_bot_have_local > NP_RING && first < g_bot_have_local - NP_RING + 1) first = g_bot_have_local - NP_RING + 1;
    int n = np_build_input(pkt, g_bot_session, g_bot_have_remote, g_bot_local, base, len, first,
                           g_bot_have_local, NULL, 0);
    if (n > 0) bot_send(pkt, n);
}

/* bot-friend: player 2 turns (H/K), fires (J) and steps forward (W) */
static void bot_friend_keys(uint32_t f, uint8_t *kb) {
    memset(kb, 0, KBD_SIZE);
    if (f < 300) return;
    uint32_t t = f % 240;
    if (t < 150) kb[0x24] = 0x80;                        /* J: fire */
    if ((f / 120) % 2 == 0) kb[0x23] = 0x80; else kb[0x25] = 0x80; /* H / K: turn */
    if (t >= 160 && t < 220) kb[0x11] = 0x80;             /* W: forward */
}

static void run_bot_friend(void) {
    say("Бот-напарник готов. В игре: главное меню -> F8 (начать сессию), затем «ДВА ИГРОКА».");
    say("Второй игрок (бот): поворачивается H/K, стреляет J, иногда идёт вперёд W. Ctrl+C - выход.");
    uint32_t chunks = 0, session = 0, delay = 0;
    int running = 0;
    DWORD last_link = 0;
    for (;;) {
        DWORD now = GetTickCount();
        if (now - last_link > 250) { send_link(); last_link = now; }
        service_tick();
        uint8_t buf[2048];
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(g_link, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) {
            if (buf[0] == PK_STATUS) { if (!g_service) print_status(buf, n); }
            else if (buf[0] == PK_CMD) handle_cmd(buf, n);
            else if (buf[0] == PK_START && n >= (int)sizeof(np_start_hdr)) {
                np_start_hdr *h = (np_start_hdr *)buf;
                if (h->session != session) { session = h->session; chunks = 0; running = 0; }
                if (h->idx < NP_START_CHUNKS && h->idx * NP_CHUNK + h->len <= sizeof(np_start)) {
                    memcpy((uint8_t *)&g_bot_start + h->idx * NP_CHUNK, buf + sizeof *h, h->len);
                    chunks |= 1u << h->idx;
                }
                np_start_ack a = { PK_START_ACK, session, chunks };
                bot_send(&a, sizeof a);
                if (!running && chunks == (1u << NP_START_CHUNKS) - 1) {
                    running = 1;
                    g_bot_session = session;
                    delay = g_bot_start.delay;
                    memset(g_bot_local, 0, sizeof g_bot_local);
                    g_bot_have_local = delay;
                    g_bot_have_remote = g_bot_peer_ack = 0;
                    say("Бот: сессия %08x, задержка %u кадров", session, delay);
                }
            } else if (buf[0] == PK_INPUT && running) {
                np_input_hdr *h = (np_input_hdr *)buf;
                if (h->session == g_bot_session &&
                    np_parse_input(buf, n, g_bot_remote, g_bot_start.base, INPUT_OBJ_SIZE, &g_bot_have_remote, NULL, NULL)) {
                    if (h->ack > g_bot_peer_ack && h->ack <= g_bot_have_local) g_bot_peer_ack = h->ack;
                    /* keep up with the host: our frames up to what it already produced */
                    /* like a real peer, run ahead by the input delay */
                    while (g_bot_have_local < g_bot_have_remote + delay) {
                        np_frame *fr = &g_bot_local[g_bot_have_local % NP_RING];
                        bot_friend_keys(g_bot_have_local, fr->blob);
                        fr->nmsg = 0;
                        g_bot_have_local++;
                    }
                    bot_send_input(g_zero, KBD_SIZE);
                }
            } else if (buf[0] == PK_STOP) { running = 0; say("Бот: сессия остановлена хостом"); }
        } else Sleep(1);
        bot_flush();
    }
}

/* bot-host: the recorded session plays as the host */
static uint8_t *g_rec;      /* frames x (u32 frame + 0x7D4) */
static uint32_t g_rec_frames;
typedef struct { uint32_t frame, msg, wparam, lparam; } rec_msg;
static rec_msg *g_rec_msgs;
static uint32_t g_rec_nmsgs;
typedef struct { uint32_t seed, calls; } rec_check;
static rec_check *g_rec_checks;
static uint32_t g_rec_nchecks, g_checks_ok, g_checks_bad, g_first_bad = UINT32_MAX, g_last_checked;

static void *read_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc(n + 1);
    if (fread(p, 1, n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    ((char *)p)[n] = 0;
    *size = (uint32_t)n;
    return p;
}

static void on_bot_check(const np_check *c, void *ctx) {
    (void)ctx;
    if (c->frame >= g_rec_nchecks || c->frame < g_last_checked) return;
    g_last_checked = c->frame + 1;
    if (c->seed == g_rec_checks[c->frame].seed && c->calls == (uint16_t)g_rec_checks[c->frame].calls) g_checks_ok++;
    else {
        g_checks_bad++;
        if (g_first_bad == UINT32_MAX) {
            g_first_bad = c->frame;
            say("Бот: РАСХОЖДЕНИЕ на кадре %u: игра seed=%08x calls=%u, запись seed=%08x calls=%u", c->frame,
                c->seed, c->calls, g_rec_checks[c->frame].seed, g_rec_checks[c->frame].calls);
        }
    }
}

static void rec_frame_blob(uint32_t f, const uint8_t *mask, uint8_t *blob) {
    const uint8_t *obj = g_rec + (size_t)f * (4 + INPUT_OBJ_SIZE) + 4;
    for (uint32_t i = 0; i < INPUT_OBJ_SIZE; i++)
        blob[i] = np_mask_get(mask, i) && !(i >= KBD_PREV_FIRST && i < KBD_PREV_END) ? obj[i] : 0;
}

static int load_recording(np_start *st) {
    uint32_t sz;
    g_rec = read_file(REC_DIR "\\input.bin", &sz);
    if (!g_rec) { say("Нет записи %s\\input.bin", REC_DIR); return 0; }
    g_rec_frames = sz / (4 + INPUT_OBJ_SIZE);
    uint8_t *m = read_file(REC_DIR "\\mask.bin", &sz);
    if (!m || sz != INPUT_OBJ_SIZE) { say("Нет mask.bin"); return 0; }
    memset(st, 0, sizeof *st);
    for (uint32_t i = 0; i < INPUT_OBJ_SIZE; i++) if (m[i]) np_mask_set(st->mask, i);
    g_rec_msgs = read_file(REC_DIR "\\msgs.bin", &sz);
    g_rec_nmsgs = g_rec_msgs ? sz / sizeof(rec_msg) : 0;
    char *hr = read_file(REC_DIR "\\hash_record.txt", &sz);
    if (!hr) { say("Нет hash_record.txt"); return 0; }
    g_rec_checks = calloc(g_rec_frames + 1, sizeof(rec_check));
    for (char *line = strtok(hr, "\n"); line; line = strtok(NULL, "\n")) {
        unsigned f, hash, seed, calls;
        if (sscanf(line, "%u %x %x %u", &f, &hash, &seed, &calls) == 4 && f < g_rec_frames) {
            g_rec_checks[f].seed = seed;
            g_rec_checks[f].calls = calls;
            if (f + 1 > g_rec_nchecks) g_rec_nchecks = f + 1;
        }
    }
    /* profiles and settings: this PC's own save.dat and config.ini (read only) */
    uint8_t *save = read_file(GAME_DIR "\\save.dat", &sz);
    if (!save || sz != GAME_PROFILES_LEN) { say("Не прочитан save.dat"); return 0; }
    memcpy(st->profiles, save, GAME_PROFILES_LEN);
    st->profile_idx = (uint8_t)GetPrivateProfileIntA("Game", "Profile", 0, GAME_DIR "\\config.ini");
    st->mouse_sens = (int32_t)GetPrivateProfileIntA("Game", "MouseSens", 38, GAME_DIR "\\config.ini");
    st->blood = (uint8_t)(GetPrivateProfileIntA("Game", "BloodDisappear", 0, GAME_DIR "\\config.ini") != 0);
    st->seed = REC_SEED;
    st->delay = 4;
    rec_frame_blob(0, st->mask, st->base);
    say("Запись: %u кадров, %u сообщений окна", g_rec_frames, g_rec_nmsgs);
    return 1;
}

static void run_bot_host(void) {
    if (!load_recording(&g_bot_start)) return;
    say("Игра должна быть только что запущена и стоять в главном меню. Мышь и клавиатуру не трогайте.");
    say("Нажмите Enter, чтобы бот-хост начал сессию...");
    DWORD last_link = 0;
    char line[16];
    /* keep the link alive while waiting for Enter */
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(in);
    for (;;) {
        if (GetTickCount() - last_link > 250) { send_link(); last_link = GetTickCount(); }
        if (WaitForSingleObject(in, 50) == WAIT_OBJECT_0) {
            INPUT_RECORD r;
            DWORD got;
            ReadConsoleInputA(in, &r, 1, &got);
            if (got && r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown && r.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) break;
        }
    }
    (void)line;
    g_bot_session = GetTickCount() | 1;
    uint32_t delay = g_bot_start.delay, acked = 0, msg_i = 0;
    memset(g_bot_local, 0, sizeof g_bot_local);
    for (uint32_t f = 0; f < delay; f++) memcpy(g_bot_local[f].blob, g_bot_start.base, INPUT_OBJ_SIZE);
    g_bot_have_local = 0; /* frames are produced from the recording below, 0.. as well */
    g_bot_have_remote = g_bot_peer_ack = 0;
    DWORD last_chunks = 0, last_send = 0, last_report = GetTickCount(), done_at = 0;
    say("Бот-хост: отправляю начальное состояние");
    for (;;) {
        DWORD now = GetTickCount();
        if (now - last_link > 250) { send_link(); last_link = now; }
        if (acked != (1u << NP_START_CHUNKS) - 1 && now - last_chunks > 150) {
            uint8_t pkt[NP_MAX_PACKET];
            for (uint32_t i = 0; i < NP_START_CHUNKS; i++) {
                if (acked & (1u << i)) continue;
                np_start_hdr *h = (np_start_hdr *)pkt;
                uint32_t off = i * NP_CHUNK, len = sizeof(np_start) - off < NP_CHUNK ? sizeof(np_start) - off : NP_CHUNK;
                h->type = PK_START; h->session = g_bot_session; h->idx = (uint16_t)i;
                h->total = (uint16_t)NP_START_CHUNKS; h->len = (uint16_t)len;
                memcpy(pkt + sizeof *h, (uint8_t *)&g_bot_start + off, len);
                bot_send(pkt, sizeof *h + len);
            }
            last_chunks = now;
        }
        uint8_t buf[2048];
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(g_link, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) {
            if (buf[0] == PK_STATUS) print_status(buf, n);
            else if (buf[0] == PK_START_ACK && n >= (int)sizeof(np_start_ack)) {
                np_start_ack *a = (np_start_ack *)buf;
                if (a->session == g_bot_session) acked |= a->have;
            } else if (buf[0] == PK_INPUT) {
                np_input_hdr *h = (np_input_hdr *)buf;
                if (h->session == g_bot_session &&
                    np_parse_input(buf, n, g_bot_remote, g_zero, KBD_SIZE, &g_bot_have_remote, on_bot_check, NULL)) {
                    if (h->ack > g_bot_peer_ack && h->ack <= g_bot_have_local) g_bot_peer_ack = h->ack;
                }
            }
        }
        /* produce recorded frames a bit ahead of the game */
        if (acked == (1u << NP_START_CHUNKS) - 1) {
            while (g_bot_have_local < g_rec_frames && g_bot_have_local < g_bot_have_remote + delay + 20) {
                uint32_t f = g_bot_have_local;
                np_frame *fr = &g_bot_local[f % NP_RING];
                rec_frame_blob(f, g_bot_start.mask, fr->blob);
                fr->nmsg = 0;
                while (msg_i < g_rec_nmsgs && g_rec_msgs[msg_i].frame <= f) {
                    if (fr->nmsg < NP_MAX_MSGS) {
                        np_msg *m = &fr->msgs[fr->nmsg++];
                        m->msg = (uint16_t)g_rec_msgs[msg_i].msg;
                        m->wparam = g_rec_msgs[msg_i].wparam;
                        m->lparam = g_rec_msgs[msg_i].lparam;
                    } else say("Бот: в кадре %u больше %d сообщений, лишние пропущены", f, NP_MAX_MSGS);
                    msg_i++;
                }
                g_bot_have_local++;
            }
            if (now - last_send >= 8) { bot_send_input(g_bot_start.base, INPUT_OBJ_SIZE); last_send = now; }
        }
        if (now - last_report > 5000) {
            say("Бот: кадр игры %u из %u, проверок совпало %u, расхождений %u", g_last_checked, g_rec_frames,
                g_checks_ok, g_checks_bad);
            last_report = now;
        }
        if (!done_at && g_last_checked + 2 >= g_rec_frames && g_rec_frames) {
            done_at = now;
            np_stop s = { PK_STOP, g_bot_session };
            for (int i = 0; i < 5; i++) bot_send(&s, sizeof s);
        }
        if (done_at && now - done_at > 1000) {
            bot_flush();
            say("ИТОГ: %u кадров проверено, совпало %u, расхождений %u%s", g_last_checked, g_checks_ok, g_checks_bad,
                g_checks_bad ? "" : " - режим друга работает детерминированно");
            return;
        }
        if (n <= 0) Sleep(1);
        bot_flush();
    }
}

/* ---- real play over the internet ---- */

static void send_hello(void) {
    np_hello h = { PK_HELLO, NP_MAGIC, NP_VERSION, (uint8_t)g_role, g_nonce, g_peer_nonce };
    if (g_connected) { sendto(g_inet, (char *)&h, sizeof h, 0, (struct sockaddr *)&g_peer, sizeof g_peer); return; }
    sendto(g_inet, (char *)&h, sizeof h, 0, (struct sockaddr *)&g_peer_pub, sizeof g_peer_pub);
    if (g_peer_lan.sin_addr.s_addr) sendto(g_inet, (char *)&h, sizeof h, 0, (struct sockaddr *)&g_peer_lan, sizeof g_peer_lan);
}

static char g_code[64];
static int g_info_state;
static HANDLE g_game;

static void send_info(void) {
    np_info i = { PK_INFO, (uint8_t)g_info_state, (uint8_t)g_role, g_rtt };
    snprintf(i.code, sizeof i.code, "%s", g_code);
    snprintf(i.note, sizeof i.note, "%s", g_note);
    to_dll(&i, sizeof i);
}

/* service: keep the game informed, and go away together with it */
static void service_tick(void) {
    static DWORD last;
    if (!g_service) return;
    if (g_game && WaitForSingleObject(g_game, 0) == WAIT_OBJECT_0) ExitProcess(0);
    if (GetTickCount() - last > 300) { send_info(); last = GetTickCount(); }
}

static void handle_cmd(const uint8_t *p, int n) {
    if (n >= 2 && p[0] == PK_CMD && p[1] == CMD_QUIT) ExitProcess(0);
}

/* STUN + own invite code (copied to the clipboard) */
static struct sockaddr_in g_pub;

/* the internet socket: the fixed port first, so most routers keep it and the code stays short */
static SOCKET bind_inet(void) {
    for (uint16_t p = NP_PORT_GAME; p < NP_PORT_GAME + 8; p++) {
        SOCKET s = udp_bind(INADDR_ANY, p);
        if (s == INVALID_SOCKET) continue;
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
        return s;
    }
    return udp_bind(INADDR_ANY, 0);
}

/* Radmin VPN gives every member an address in 26.0.0.0/8.  A code with that
   address is enough: both sides send UDP directly through the virtual adapter.
   RIP3COOP_LOCAL_IP is an escape hatch for testing and for other VPNs. */
static uint32_t find_radmin_ip(void) {
    const char *env = getenv("RIP3COOP_LOCAL_IP");
    if (env && *env) {
        struct in_addr a;
        if (inet_pton(AF_INET, env, &a) == 1) return ntohl(a.s_addr);
    }
    uint32_t found = 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    INTERFACE_INFO info[32];
    DWORD bytes = 0;
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, info, sizeof info, &bytes, NULL, NULL) == 0) {
        DWORD n = bytes / sizeof info[0];
        for (DWORD i = 0; i < n; i++) {
            if (!(info[i].iiFlags & IFF_UP)) continue;
            uint32_t ip = ntohl(info[i].iiAddress.AddressIn.sin_addr.s_addr);
            if ((ip >> 24) == 26) { found = ip; break; }
        }
    }
    closesocket(s);
    return found;
}

/* STUN or Radmin address, plus own invite code (copied to the clipboard) */
static int make_identity(void) {
    srand(GetTickCount() ^ GetCurrentProcessId());
    g_nonce = (uint16_t)(rand() ^ (rand() << 4)) | 1;
    g_code[0] = 0;

    if (g_radmin) {
        uint32_t ip = find_radmin_ip();
        if (!ip) {
            say("Radmin VPN не найден. Включите Radmin и повторите.");
            return 0;
        }
        struct sockaddr_in local = { 0 };
        int al = sizeof local;
        if (g_inet == INVALID_SOCKET || getsockname(g_inet, (struct sockaddr *)&local, &al) != 0) {
            say("Не удалось открыть UDP-порт для Radmin.");
            return 0;
        }
        memset(&g_pub, 0, sizeof g_pub);
        g_pub.sin_family = AF_INET;
        g_pub.sin_addr.s_addr = htonl(ip);
        g_pub.sin_port = local.sin_port;
        encode_code(ip, ntohs(g_pub.sin_port), g_code);
        copy_to_clipboard(g_code);
        say("Radmin VPN: код скопирован (порт %u). Разрешите UDP 27605 в брандмауэре.",
            (unsigned)ntohs(g_pub.sin_port));
        return 1;
    }

    say("Узнаю свой внешний адрес...");
    struct sockaddr_in pub2 = { 0 };
    int have_pub = stun_query("stun.l.google.com", &g_pub) || stun_query("stun1.l.google.com", &g_pub);
    int symmetric = have_pub && stun_query("stun2.l.google.com", &pub2) && pub2.sin_port != g_pub.sin_port;
    if (!have_pub) {
        /* no internet answer: the code carries the LAN address */
        g_pub.sin_addr.s_addr = lan_ip();
        struct sockaddr_in local = { 0 };
        int al = sizeof local;
        getsockname(g_inet, (struct sockaddr *)&local, &al);
        g_pub.sin_port = local.sin_port;
    }
    encode_code(ntohl(g_pub.sin_addr.s_addr), ntohs(g_pub.sin_port), g_code);
    copy_to_clipboard(g_code);
    if (!have_pub) say("Внешний адрес не найден - код сработает только в одной локальной сети.");
    else if (symmetric) say("Внимание: строгий NAT у роутера - соединение может не получиться.");
    else say("Ваш код скопирован.");
    return 1;
}

static int set_peer(const char *text) {
    uint32_t ip;
    uint16_t port;
    if (!decode_code(text, &ip, &port)) { say("Код не распознан, проверьте его."); return 0; }
    if (htonl(ip) == g_pub.sin_addr.s_addr && htons(port) == g_pub.sin_port && g_code[0] &&
        strspn(text, " \t") < strlen(text) && strstr(text, g_code)) { say("Это ваш собственный код - нужен код друга."); return 0; }
    g_peer_pub.sin_family = AF_INET;
    g_peer_pub.sin_addr.s_addr = htonl(ip);
    g_peer_pub.sin_port = htons(port);
    /* same public address = same home network: find each other by LAN broadcast */
    memset(&g_peer_lan, 0, sizeof g_peer_lan);
    if (g_peer_pub.sin_addr.s_addr == g_pub.sin_addr.s_addr) {
        g_peer_lan.sin_family = AF_INET;
        g_peer_lan.sin_addr.s_addr = INADDR_BROADCAST;
        g_peer_lan.sin_port = htons(NP_PORT_GAME);
    }
    return 1;
}

static void online_loop(void) {
    g_info_state = INFO_CONNECTING;
    say("Соединяюсь с другом...");
    DWORD last_hello = 0, last_ping = 0, last_link = 0, t_start = GetTickCount(), last_wait_msg = GetTickCount();
    int lost_reported = 0;
    for (;;) {
        DWORD now = GetTickCount();
        service_tick();
        if (now - last_hello > (DWORD)(g_connected ? 1000 : 200)) { send_hello(); last_hello = now; }
        if (g_connected && now - last_ping > 1000) {
            np_ping p = { PK_PING, now };
            sendto(g_inet, (char *)&p, sizeof p, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
            last_ping = now;
        }
        if (now - last_link > 250) { send_link(); last_link = now; }
        if (!g_connected && now - last_wait_msg > 15000) {
            say("Жду друга (%lu с). Он тоже должен ввести ваш код.", (now - t_start) / 1000);
            last_wait_msg = now;
        }
        if (g_connected && now - g_last_peer > 5000 && !lost_reported) { say("Друг не отвечает..."); lost_reported = 1; }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_inet, &rd);
        FD_SET(g_link, &rd);
        struct timeval tv = { 0, 5000 };
        select(0, &rd, NULL, NULL, &tv);
        uint8_t buf[2048];
        struct sockaddr_in from;
        int fl, n;
        while (fl = sizeof from, (n = recvfrom(g_inet, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0) {
            if (buf[0] == PK_HELLO && n >= (int)sizeof(np_hello)) {
                np_hello *h = (np_hello *)buf;
                if (h->magic != NP_MAGIC || h->nonce == g_nonce) continue; /* our own LAN broadcast */
                if (!g_connected && from.sin_addr.s_addr != g_peer_pub.sin_addr.s_addr && !g_peer_lan.sin_family) continue;
                if (h->version != NP_VERSION) { say("У друга другая версия мода."); continue; }
                if (h->role == g_role) { say("Вы оба выбрали одну роль: один создаёт игру, другой подключается."); continue; }
                if (!g_connected) {
                    g_connected = 1;
                    g_peer = from;
                    g_info_state = INFO_CONNECTED;
                    say(g_role == ROLE_HOST ? "Соединено! Когда оба в главном меню - F8, затем «ДВА ИГРОКА»."
                                            : "Соединено! Ждите в главном меню, хост начнёт. Вы: WASD, H/K поворот, J огонь, U доп.");
                    send_hello();
                }
            }
            if (!g_connected || from.sin_addr.s_addr != g_peer.sin_addr.s_addr || from.sin_port != g_peer.sin_port) continue;
            g_last_peer = GetTickCount();
            if (lost_reported) { say("Друг снова на связи"); lost_reported = 0; }
            if (buf[0] == PK_PING && n >= (int)sizeof(np_ping)) { buf[0] = PK_PONG; sendto(g_inet, (char *)buf, n, 0, (struct sockaddr *)&from, fl); }
            else if (buf[0] == PK_PONG && n >= (int)sizeof(np_ping)) {
                uint16_t rtt = (uint16_t)(GetTickCount() - ((np_ping *)buf)->t);
                if (!g_rtt && !g_service) say("Пинг: %u мс", rtt);
                g_rtt = rtt;
            } else if (buf[0] >= 0x10 && buf[0] < 0x80) to_dll(buf, n);
        }
        while (fl = sizeof from, (n = recvfrom(g_link, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0) {
            if (buf[0] == PK_STATUS) { if (!g_service) print_status(buf, n); }
            else if (buf[0] == PK_CMD) handle_cmd(buf, n);
            else if (buf[0] >= 0x10 && buf[0] < 0x80 && g_connected)
                sendto(g_inet, (char *)buf, n, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
        }
    }
}

static void run_online(void) {
    if (!make_identity()) return;
    printf("\n  ВАШ КОД:  %s\n  (уже скопирован - отправьте его %s)\n\n", g_code, g_role == ROLE_HOST ? "другу" : "хосту");
    for (;;) {
        printf("Вставьте код %s и нажмите Enter: ", g_role == ROLE_HOST ? "друга" : "хоста");
        fflush(stdout);
        char line[128];
        read_line(line, sizeof line);
        if (set_peer(line)) break;
    }
    online_loop();
}

static void service_idle(void) {
    static DWORD last;
    service_tick();
    if (GetTickCount() - last > 250) { send_link(); last = GetTickCount(); }
}

/* rip3coop.exe --service <game pid>: no window, driven by the in-game panel (F7) */
static void run_service(DWORD pid) {
    g_service = 1;
    g_game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    g_idle_hook = service_idle;
    g_info_state = INFO_IDLE;
    say("Кооператив: 1 - создать игру, 2 - подключиться к другу");
    for (DWORD last_link = 0;;) {
        service_tick();
        if (GetTickCount() - last_link > 250) { send_link(); last_link = GetTickCount(); }
        uint8_t buf[256];
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(g_link, (char *)buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n < 2 || buf[0] != PK_CMD) { Sleep(10); continue; }
        np_cmd *c = (np_cmd *)buf;
        buf[n] = 0;
        if (c->op == CMD_QUIT) ExitProcess(0);
        if ((c->op == CMD_HOST || c->op == CMD_JOIN) && g_info_state == INFO_IDLE) {
            g_role = c->op == CMD_HOST ? ROLE_HOST : ROLE_CLIENT;
            g_radmin = !strcmp(c->text, "radmin");
            if (g_inet == INVALID_SOCKET) g_inet = bind_inet();
            if (!make_identity()) {
                g_info_state = INFO_ERROR;
                send_info();
            } else {
                g_info_state = INFO_NEED_CODE;
                say("Ваш код скопирован - отправьте его %s. Затем скопируйте его код и нажмите Ctrl+V.",
                    g_role == ROLE_HOST ? "другу" : "хосту");
            }
        } else if (c->op == CMD_PEER_CODE && g_info_state == INFO_NEED_CODE) {
            c->text[sizeof c->text - 1] = 0;
            if (set_peer(c->text)) online_loop();
        } else if (c->op == CMD_BOT_FRIEND && g_info_state == INFO_IDLE) {
            g_role = ROLE_HOST;
            g_connected = 1;
            g_rtt = 60;
            g_info_state = INFO_CONNECTED;
            say("Тест: бот играет вторым игроком. F8 - начать сессию, затем «ДВА ИГРОКА».");
            run_bot_friend();
        }
    }
}

int main(int argc, char **argv) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    *strrchr(exe, '\\') = 0;
    SetCurrentDirectoryA(exe); /* REC_DIR is relative to build\ */
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srand(GetTickCount());
    g_dll.sin_family = AF_INET;
    g_dll.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_dll.sin_port = htons(NP_PORT_DLL);
    g_link = udp_bind(htonl(INADDR_LOOPBACK), NP_PORT_LAUNCHER);
    if (argc >= 3 && !strcmp(argv[1], "--service")) {
        if (g_link == INVALID_SOCKET) return 1; /* another launcher already serves the game */
        run_service((DWORD)strtoul(argv[2], NULL, 10));
        return 0;
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("RIP 3 co-op");
    if (g_link == INVALID_SOCKET) {
        printf("rip3coop уже запущен (порт %d занят). Кооп теперь можно вести из игры: F7.\n", NP_PORT_LAUNCHER);
        Sleep(5000);
        return 1;
    }

    printf("RIP 3 - сетевой кооператив (то же самое есть в игре: F7)\n"
           "  1 - Создать игру (вы хост, первый игрок)\n"
           "  2 - Подключиться к другу (второй игрок)\n"
           "  3 - Тест: бот-напарник (вы хост, бот - второй игрок)\n"
           "  4 - Тест: бот-хост (повтор записи, проверка режима друга)\n"
           "Выбор: ");
    fflush(stdout);
    char line[16];
    read_line(line, sizeof line);
    switch (line[0]) {
    case '1': g_role = ROLE_HOST; break;
    case '2': g_role = ROLE_CLIENT; break;
    case '3': g_role = ROLE_HOST; g_connected = 1; g_rtt = 60; run_bot_friend(); return 0;
    case '4': g_role = ROLE_CLIENT; g_connected = 1; g_rtt = 60; run_bot_host(); printf("Enter - выход"); read_line(line, sizeof line); return 0;
    default: return 0;
    }
    g_inet = bind_inet();
    run_online();
    return 0;
}

