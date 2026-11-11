/* sli_enc.c - the .sli encoder (sli_enc.h): RGBA8 rows to a .sli file, the rule of phpbm's SliEncoder. */
#include "slate_speed.h"   /* first: the code below is built for speed */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sli_enc.h"

#define SLI_MAX 32767
#define MIN_REPEAT 4   /* shortest series of one colour written as a "repeated" command */

enum { K_EMPTY = 0, K_REPEAT = 1, K_DIFFERENT = 2, K_SEMI = 3 };

/* The commands are kept in pieces of PIECE bytes (header included), allocated as they fill. */
#define PIECE 4096
typedef struct piece {
    struct piece *next;
    size_t used;
    uint8_t d[];
} piece_t;
#define PIECE_CAP (PIECE - offsetof(piece_t, d))

struct slate_sli_enc {
    void *(*alloc)(size_t);
    void (*release)(void *);
    int w, h, y;          /* y: rows given so far */
    bool failed, opaque;
    uint32_t *table;      /* each row's offset in the data */
    size_t len;           /* bytes of data so far */
    piece_t *head, *tail;
};

/* The 4x4 ordered dither thresholds of gfx_sw.c. */
static const uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* x / 255 rounded to nearest, for 0 <= x <= 65535 (slate_gfx.h). */
static inline unsigned div255(unsigned v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

/* An opaque premultiplied colour reduced to RGB565 with dither threshold th (0..15), as pm_opaque in gfx_sw.c. */
static inline uint16_t pm_opaque(unsigned r, unsigned g, unsigned b, unsigned th)
{
    unsigned r5 = (r + (th >> 1)) >> 3, g6 = (g + (th >> 2)) >> 2, b5 = (b + (th >> 1)) >> 3;
    if (r5 > 31) r5 = 31;
    if (g6 > 63) g6 = 63;
    if (b5 > 31) b5 = 31;
    return (uint16_t)(r5 << 11 | g6 << 5 | b5);
}

static inline int kind_of(uint8_t a) { return a == 0 ? K_EMPTY : a == 255 ? K_REPEAT : K_SEMI; }

/* The .sli colour of pixel x (not empty) of a row whose dither row is bay. */
static inline uint16_t color_at(const uint8_t *rgba, const uint8_t *bay, int x)
{
    const uint8_t *p = rgba + 4 * (size_t)x;
    unsigned a = p[3], th = bay[x & 3];
    if (a == 255) {
        return pm_opaque(p[0], p[1], p[2], th);
    }
    return pm_opaque(div255(p[0] * a), div255(p[1] * a), div255(p[2] * a), th);
}

/* ---- output: bytes appended to the pieces; a failed allocation sets failed and drops what follows ---- */

static void put_slow(slate_sli_enc_t *e, uint8_t b)
{
    if (e->failed) {
        return;
    }
    piece_t *p = e->alloc(PIECE);
    if (!p) {
        e->failed = true;
        return;
    }
    p->next = NULL;
    p->used = 0;
    if (e->tail) {
        e->tail->next = p;
    } else {
        e->head = p;
    }
    e->tail = p;
    p->d[p->used++] = b;
    e->len++;
}

static inline void put(slate_sli_enc_t *e, uint8_t b)
{
    piece_t *t = e->tail;
    if (t && t->used < PIECE_CAP) {
        t->d[t->used++] = b;
        e->len++;
    } else {
        put_slow(e, b);
    }
}

static void command(slate_sli_enc_t *e, int kind, int n)
{
    if (n <= 63) {
        put(e, (uint8_t)(kind << 6 | n));
    } else {
        put(e, (uint8_t)(kind << 6));
        put(e, (uint8_t)n);
        put(e, (uint8_t)(n >> 8));
    }
}

/* The padding byte that puts what follows at an even offset from the start of the data. */
static inline void align2(slate_sli_enc_t *e)
{
    if (e->len & 1) {
        put(e, 0);
    }
}

static void colors(slate_sli_enc_t *e, const uint8_t *rgba, const uint8_t *bay, int s, int end)
{
    for (int x = s; x < end; x++) {
        uint16_t c = color_at(rgba, bay, x);
        put(e, (uint8_t)c);
        put(e, (uint8_t)(c >> 8));
    }
}

/* A "different" command for pixels s..end - 1, if any. */
static void different(slate_sli_enc_t *e, const uint8_t *rgba, const uint8_t *bay, int s, int end)
{
    if (s == end) {
        return;
    }
    command(e, K_DIFFERENT, end - s);
    align2(e);
    colors(e, rgba, bay, s, end);
}

/* The commands of the opaque pixels s..end - 1. */
static void opaque_span(slate_sli_enc_t *e, const uint8_t *rgba, const uint8_t *bay, int s, int end)
{
    int pending = s;   /* first pixel not written yet */
    for (int i = s; i < end;) {
        uint16_t c = color_at(rgba, bay, i);
        int j = i + 1;
        while (j < end && color_at(rgba, bay, j) == c) {
            j++;
        }
        if (j - i >= MIN_REPEAT) {
            different(e, rgba, bay, pending, i);
            command(e, K_REPEAT, j - i);
            put(e, (uint8_t)c);
            put(e, (uint8_t)(c >> 8));
            pending = j;
        }
        i = j;
    }
    different(e, rgba, bay, pending, end);
}

slate_sli_enc_t *slate_sli_enc_new(int w, int h, void *(*alloc)(size_t), void (*release)(void *))
{
    if (w < 1 || h < 1 || w > SLI_MAX || h > SLI_MAX) {
        return NULL;
    }
    slate_sli_enc_t *e = alloc(sizeof *e);
    if (!e) {
        return NULL;
    }
    memset(e, 0, sizeof *e);
    e->alloc = alloc;
    e->release = release;
    e->w = w;
    e->h = h;
    e->opaque = true;
    e->table = alloc(4 * (size_t)h);
    if (!e->table) {
        release(e);
        return NULL;
    }
    return e;
}

bool slate_sli_enc_row(slate_sli_enc_t *e, const uint8_t *rgba)
{
    if (e->failed || e->y >= e->h) {
        e->failed = true;
        return false;
    }
    while (e->len & 3) {
        put(e, 0);
    }
    e->table[e->y] = (uint32_t)e->len;
    const uint8_t *bay = BAYER4[e->y & 3];
    int n = e->w;
    for (int s = 0; s < n;) {
        int kind = kind_of(rgba[4 * (size_t)s + 3]);
        int end = s + 1;
        while (end < n && end - s < SLI_MAX && kind_of(rgba[4 * (size_t)end + 3]) == kind) {
            end++;
        }
        switch (kind) {
        case K_EMPTY:
            e->opaque = false;
            command(e, K_EMPTY, end - s);
            break;
        case K_SEMI:
            e->opaque = false;
            command(e, K_SEMI, end - s);
            align2(e);
            colors(e, rgba, bay, s, end);
            for (int x = s; x < end; x++) {
                put(e, rgba[4 * (size_t)x + 3]);
            }
            break;
        default:
            opaque_span(e, rgba, bay, s, end);
        }
        s = end;
    }
    /* the data size is a u32: reachable only where size_t is 64 bits (the host; a 32767 x 32767 image can need
       more than 4 GB); with a 32-bit size_t memory runs out long before (and the compare would always be false) */
#if SIZE_MAX > UINT32_MAX
    if (e->len > UINT32_MAX) {
        e->failed = true;
    }
#endif
    e->y++;
    return !e->failed;
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

void slate_sli_enc_abort(slate_sli_enc_t *e)
{
    e->failed = true;
    size_t size;
    slate_sli_enc_finish(e, &size);
}

uint8_t *slate_sli_enc_finish(slate_sli_enc_t *e, size_t *size)
{
    uint8_t *out = NULL;
    size_t total = 16 + 4 * (size_t)e->h + e->len;
    if (!e->failed && e->y == e->h) {
        out = e->alloc(total);
    }
    if (out) {
        memcpy(out, "SLI1", 4);
        out[4] = 1;
        out[5] = e->opaque ? 2 : 1;
        put16(out + 6, (unsigned)e->w);
        put16(out + 8, (unsigned)e->h);
        put16(out + 10, 0);
        put32(out + 12, (uint32_t)e->len);
        for (int y = 0; y < e->h; y++) {
            put32(out + 16 + 4 * (size_t)y, e->table[y]);
        }
        size_t at = 16 + 4 * (size_t)e->h;
        for (piece_t *p = e->head; p; p = p->next) {
            memcpy(out + at, p->d, p->used);
            at += p->used;
        }
    }
    for (piece_t *p = e->head; p;) {
        piece_t *next = p->next;
        e->release(p);
        p = next;
    }
    e->release(e->table);
    e->release(e);
    *size = out ? total : 0;
    return out;
}
