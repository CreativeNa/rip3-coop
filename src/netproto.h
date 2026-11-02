// netproto.h - wire format shared by rip3net.dll (inside the game) and rip3coop.exe
// (the launcher that holds the internet socket, and the test bots).
//
// Lockstep: both games run the same fixed 60 Hz frames from the same seed. Frame N runs
// only when both inputs for N are known. Each side samples its own input during frame N
// and sends it as the input for frame N + delay.
//   host input   = the masked DirectInput object (mouse, keyboard, joysticks) + window messages
//   client input = the 256-byte DirectInput keyboard state (player 2 keys)
// The merged input is the host input with the two keyboards OR'ed, exactly like two
// people on one keyboard in the local co-op mode.
#pragma once
#include <stdint.h>
#include <string.h>

#define NP_MAGIC          0x50433352u /* "R3CP" */
#define NP_VERSION        1
#define NP_PORT_LAUNCHER  27600       /* 127.0.0.1: launcher end of the local link */
#define NP_PORT_DLL       27601       /* 127.0.0.1: game end of the local link */
#define NP_MAX_PACKET     1200

#define INPUT_OBJ_SIZE    0x7D4u
#define KBD_VALID         0x0Du       /* keyboard object +5: GetDeviceState succeeded */
#define KBD_FIRST         0x0Fu       /* 256-byte current key state */
#define KBD_PREV_FIRST    0x10Fu      /* 256-byte previous key state (kept by the game) */
#define KBD_PREV_END      0x20Fu
#define KBD_SIZE          256u

#define NP_RING           256         /* frames kept per side; must exceed delay + resend window */
#define NP_MAX_MSGS       32
#define MSG_MOUSE_ITEM    0xFFFFu     /* np_msg carrying a raw mouse event: wparam = offset, lparam = data */

/* game object fields synced at session start (FUN_00401520 / FUN_00407b90) */
#define GAME_PROFILES     0xE8E0u     /* 6 profiles x 0x13C, the contents of save.dat */
#define GAME_PROFILES_LEN 0x768u
#define GAME_PROFILE_IDX  0xF048u     /* byte */
#define GAME_MOUSE_SENS   0x155C0u    /* int */
#define GAME_BLOOD        0xB21u      /* byte */

enum {
    /* launcher <-> launcher, never forwarded to the game */
    PK_HELLO = 1, PK_PING = 2, PK_PONG = 3,
    /* game <-> game, forwarded by the launchers */
    PK_START = 0x10, PK_START_ACK = 0x11, PK_INPUT = 0x12, PK_STOP = 0x13,
    PK_RESYNC = 0x14, PK_RESYNC_ACK = 0x15,
    /* launcher <-> its own game over 127.0.0.1 */
    PK_LINK = 0x80, PK_STATUS = 0x81,
    /* game -> launcher started as a hidden service (rip3coop.exe --service) */
    PK_CMD = 0x82, PK_INFO = 0x83,
};
enum { CMD_HOST = 1, CMD_JOIN, CMD_PEER_CODE, CMD_QUIT, CMD_BOT_FRIEND };
enum { INFO_IDLE, INFO_NEED_CODE, INFO_CONNECTING, INFO_CONNECTED, INFO_ERROR };
enum { ROLE_NONE, ROLE_HOST, ROLE_CLIENT };

#pragma pack(push, 1)
typedef struct { uint8_t type; uint32_t magic; uint8_t version, role; uint16_t nonce, peer_nonce; } np_hello;
typedef struct { uint8_t type; uint32_t t; } np_ping;
typedef struct { uint8_t type; uint8_t role, connected; uint16_t rtt_ms; } np_link;
typedef struct { uint8_t type; char text[200]; } np_status;
typedef struct { uint8_t type, op; char text[64]; } np_cmd;
typedef struct { uint8_t type, state, role; uint16_t rtt_ms; char code[40]; char note[160]; } np_info;
typedef struct { uint16_t msg; uint32_t wparam, lparam; } np_msg;

/* START is sent in chunks (PK_START: header + up to NP_CHUNK bytes) */
typedef struct {
    uint32_t seed;
    uint8_t delay;
    uint8_t mask[(INPUT_OBJ_SIZE + 7) / 8];   /* bit set = data byte of the input object */
    uint8_t base[INPUT_OBJ_SIZE];             /* host input "before frame 0" (diff base) */
    uint8_t profiles[GAME_PROFILES_LEN];
    uint8_t profile_idx, blood;
    int32_t mouse_sens;
} np_start;
#define NP_CHUNK 900
#define NP_START_CHUNKS ((sizeof(np_start) + NP_CHUNK - 1) / NP_CHUNK)
typedef struct { uint8_t type; uint32_t session; uint16_t idx, total, len; } np_start_hdr;
typedef struct { uint8_t type; uint32_t session; uint32_t have; } np_start_ack; /* bit per chunk */
typedef struct { uint8_t type; uint32_t session; } np_stop;

/* RESYNC: at every level load the host sends the two players and the profiles; the friend
   waits inside the same level load and takes them (dwords the host marked as pointers are
   skipped). Repairs any drift once per level. */
#define GAME_PLAYERS      0x1738u     /* 6 players x 0x22DC; co-op uses the first two */
#define GAME_PLAYER_SIZE  0x22DCu
#define RESYNC_PLAYERS    (2 * GAME_PLAYER_SIZE)
typedef struct {
    uint32_t seed;
    uint8_t players[RESYNC_PLAYERS];
    uint8_t pointers[RESYNC_PLAYERS / 4 / 8 + 1];   /* bit per dword: host value is a pointer */
    uint8_t profiles[GAME_PROFILES_LEN];
} np_resync;
#define NP_RESYNC_CHUNKS ((sizeof(np_resync) + NP_CHUNK - 1) / NP_CHUNK)
typedef struct { uint8_t type; uint32_t session, id; uint16_t idx, len; } np_resync_hdr;
typedef struct { uint8_t type; uint32_t session, id; uint64_t have; } np_resync_ack;

/* PK_INPUT: header, then nframes x { u16 diff_len, diff, u8 nmsg, nmsg x np_msg },
   then u8 nchecks, nchecks x np_check */
typedef struct { uint8_t type; uint32_t session; uint32_t ack; uint32_t first; uint8_t nframes; } np_input_hdr;
typedef struct { uint32_t frame, seed; uint16_t calls; } np_check;
#pragma pack(pop)

typedef struct {
    uint8_t blob[INPUT_OBJ_SIZE];
    uint8_t nmsg;
    np_msg msgs[NP_MAX_MSGS];
} np_frame;

static inline int np_mask_get(const uint8_t *mask, uint32_t i) { return (mask[i >> 3] >> (i & 7)) & 1; }
static inline void np_mask_set(uint8_t *mask, uint32_t i) { mask[i >> 3] |= (uint8_t)(1u << (i & 7)); }

/* Diff of cur against prev: runs of { u16 offset, u8 len, len bytes }, ended by offset 0xFFFF.
   Returns the encoded size, or -1 if it does not fit into cap. */
static int np_diff(const uint8_t *prev, const uint8_t *cur, uint32_t n, uint8_t *out, int cap) {
    int w = 0;
    uint32_t i = 0;
    while (i < n) {
        if (prev[i] == cur[i]) { i++; continue; }
        uint32_t start = i, end = i + 1;
        /* extend the run, also over gaps of < 3 equal bytes (a run header costs 3 bytes) */
        while (end < n && end - start < 255) {
            if (prev[end] != cur[end]) { end++; continue; }
            uint32_t next = end;
            while (next < n && next - end < 3 && prev[next] == cur[next]) next++;
            if (next < n && next - end < 3 && next - start < 255) end = next; /* differs at next */
            else break;
        }
        uint32_t len = end - start;
        if (w + 3 + (int)len > cap) return -1;
        out[w++] = (uint8_t)start; out[w++] = (uint8_t)(start >> 8); out[w++] = (uint8_t)len;
        memcpy(out + w, cur + start, len);
        w += len;
        i = end;
    }
    if (w + 2 > cap) return -1;
    out[w++] = 0xFF; out[w++] = 0xFF;
    return w;
}

/* Applies a diff to blob in place. Returns the bytes consumed, or -1 on malformed input. */
static int np_undiff(uint8_t *blob, uint32_t n, const uint8_t *in, int avail) {
    int r = 0;
    for (;;) {
        if (r + 2 > avail) return -1;
        uint32_t off = in[r] | (in[r + 1] << 8);
        r += 2;
        if (off == 0xFFFF) return r;
        if (r + 1 > avail) return -1;
        uint32_t len = in[r++];
        if (off + len > n || r + (int)len > avail) return -1;
        memcpy(blob + off, in + r, len);
        r += len;
    }
}

/* Appends frames [first, last) of ring to an INPUT packet. prev(first) is the frame
   before it, or base for frame 0. Returns the packet size. */
static int np_build_input(uint8_t *pkt, uint32_t session, uint32_t ack, const np_frame *ring,
                          const uint8_t *base, uint32_t blob_len, uint32_t first, uint32_t last,
                          const np_check *checks, int nchecks) {
    np_input_hdr *h = (np_input_hdr *)pkt;
    h->type = PK_INPUT; h->session = session; h->ack = ack; h->first = first; h->nframes = 0;
    int w = sizeof *h;
    int reserve = 1 + nchecks * (int)sizeof(np_check);
    for (uint32_t f = first; f < last && h->nframes < 255; f++) {
        const np_frame *cur = &ring[f % NP_RING];
        const uint8_t *prev = f == 0 ? base : ring[(f - 1) % NP_RING].blob;
        int room = NP_MAX_PACKET - reserve - w - 2 - 1 - cur->nmsg * (int)sizeof(np_msg);
        if (room <= 2) break;
        int d = np_diff(prev, cur->blob, blob_len, pkt + w + 2, room);
        if (d < 0) { if (h->nframes == 0) return -1; break; }
        pkt[w] = (uint8_t)d; pkt[w + 1] = (uint8_t)(d >> 8);
        w += 2 + d;
        pkt[w++] = cur->nmsg;
        memcpy(pkt + w, cur->msgs, cur->nmsg * sizeof(np_msg));
        w += cur->nmsg * sizeof(np_msg);
        h->nframes++;
    }
    pkt[w++] = (uint8_t)nchecks;
    memcpy(pkt + w, checks, nchecks * sizeof(np_check));
    w += nchecks * sizeof(np_check);
    return w;
}

/* Decodes the frames of an INPUT packet into ring. *have = number of contiguous frames
   already known; it is advanced. Checks are passed to on_check. Returns 0 if malformed. */
static int np_parse_input(const uint8_t *pkt, int len, np_frame *ring, const uint8_t *base,
                          uint32_t blob_len, uint32_t *have,
                          void (*on_check)(const np_check *c, void *ctx), void *ctx) {
    if (len < (int)sizeof(np_input_hdr)) return 0;
    const np_input_hdr *h = (const np_input_hdr *)pkt;
    int r = sizeof *h;
    for (uint32_t k = 0; k < h->nframes; k++) {
        uint32_t f = h->first + k;
        if (r + 2 > len) return 0;
        int d = pkt[r] | (pkt[r + 1] << 8);
        r += 2;
        if (r + d + 1 > len) return 0;
        if (f == *have && f + NP_RING > *have) {
            np_frame *cur = &ring[f % NP_RING];
            const uint8_t *prev = f == 0 ? base : ring[(f - 1) % NP_RING].blob;
            uint8_t tmp[INPUT_OBJ_SIZE];
            memcpy(tmp, prev, blob_len);
            if (np_undiff(tmp, blob_len, pkt + r, d) != d) return 0;
            memcpy(cur->blob, tmp, blob_len);
            r += d;
            uint8_t nm = pkt[r++];
            if (nm > NP_MAX_MSGS || r + nm * (int)sizeof(np_msg) > len) return 0;
            cur->nmsg = nm;
            memcpy(cur->msgs, pkt + r, nm * sizeof(np_msg));
            r += nm * sizeof(np_msg);
            (*have)++;
        } else {
            r += d;
            uint8_t nm = pkt[r++];
            r += nm * sizeof(np_msg);
            if (r > len) return 0;
        }
    }
    if (r + 1 > len) return 0;
    int nc = pkt[r++];
    if (r + nc * (int)sizeof(np_check) > len) return 0;
    for (int i = 0; i < nc && on_check; i++) {
        np_check c;
        memcpy(&c, pkt + r + i * sizeof c, sizeof c);
        on_check(&c, ctx);
    }
    return 1;
}
