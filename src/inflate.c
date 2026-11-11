/* inflate.c - a small streaming zlib/deflate decoder (inflate.h). Written for Slate after RFC 1950 and RFC 1951;
   the canonical Huffman decoding follows the method of Mark Adler's puff (no code taken from it). */
#include "slate_speed.h"   /* first: the PNG decoder spends its time here */
#include "slate_config.h"

#if SLATE_HAS_PNG

#include <string.h>

#include "inflate.h"

#define WMASK (SLATE_INFLATE_WINDOW - 1)
#define MAXBITS 15

/* ---- bits ---- */

static int fail(slate_inflate_t *z, int err)
{
    if (!z->err) {
        z->err = err;
    }
    return -1;
}

/* Up to n bits in the buffer (fewer at the end of the input). */
static inline void fill(slate_inflate_t *z, int n)
{
    while (z->nbits < n) {
        int c = z->in(z->ctx);
        if (c < 0) {
            return;
        }
        z->bits |= (uint32_t)c << z->nbits;
        z->nbits += 8;
    }
}

/* The next n bits (0..16), first bit lowest; 0 and an error when the input ends. */
static inline unsigned bits(slate_inflate_t *z, int n)
{
    fill(z, n);
    if (z->nbits < n) {
        fail(z, SLATE_INFLATE_INPUT);
        return 0;
    }
    unsigned v = z->bits & ((1u << n) - 1);
    z->bits >>= n;
    z->nbits -= n;
    return v;
}

/* ---- output: the window, handed out when full and at the end, with the Adler-32 of everything handed out ---- */

static void flush(slate_inflate_t *z)
{
    if (z->err) {
        z->pos = 0;   /* nothing more is handed out; pos stays inside the window */
        return;
    }
    if (z->pos == 0) {
        return;
    }
    uint32_t a = z->adler_a, b = z->adler_b;
    const uint8_t *p = z->window;
    for (uint32_t left = z->pos; left;) {
        uint32_t n = left < 5552 ? left : 5552;   /* the most bytes before b can pass 2^32 */
        left -= n;
        while (n--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    z->adler_a = a;
    z->adler_b = b;
    if (!z->out(z->ctx, z->window, z->pos)) {
        fail(z, SLATE_INFLATE_OUTPUT);
    }
    if (z->pos == SLATE_INFLATE_WINDOW) {
        z->full = true;
    }
    z->pos = 0;
}

static inline void emit(slate_inflate_t *z, uint8_t c)
{
    if (z->err) {
        return;
    }
    z->window[z->pos++] = c;
    if (z->pos == SLATE_INFLATE_WINDOW) {
        flush(z);
    }
}

/* ---- Huffman codes ---- */

/* Builds h from the code lengths of n symbols. Returns < 0 for an over-subscribed code, > 0 for an incomplete one,
   0 for a complete one (or no code at all: then decoding fails). */
static int construct(slate_inflate_code_t *h, const uint8_t *length, int n)
{
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int s = 0; s < n; s++) {
        h->count[length[s]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left;
        }
    }
    uint16_t offs[MAXBITS + 2], next[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    }
    unsigned code = 0;
    next[0] = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code = (code + (len > 1 ? h->count[len - 1] : 0)) << 1;
        next[len] = (uint16_t)code;
    }
    for (int s = 0; s < n; s++) {
        int len = length[s];
        if (!len) {
            continue;
        }
        h->symbol[offs[len]++] = (uint16_t)s;
        unsigned c = next[len]++;
        if (len <= SLATE_INFLATE_FAST) {
            unsigned rev = 0;
            for (int i = 0; i < len; i++) {
                rev |= ((c >> i) & 1u) << (len - 1 - i);
            }
            for (unsigned k = rev; k < (1u << SLATE_INFLATE_FAST); k += 1u << len) {
                h->fast[k] = (uint16_t)(len << 9 | s);
            }
        }
    }
    return left;
}

/* The next symbol of code h; < 0 (with an error) when the bits are not a code or the input ends. */
static int decode(slate_inflate_t *z, const slate_inflate_code_t *h)
{
    fill(z, SLATE_INFLATE_FAST);
    unsigned e = h->fast[z->bits & ((1u << SLATE_INFLATE_FAST) - 1)];
    if (e) {
        int len = (int)(e >> 9);
        if (len > z->nbits) {
            return fail(z, SLATE_INFLATE_INPUT);
        }
        z->bits >>= len;
        z->nbits -= len;
        return (int)(e & 511);
    }
    /* a longer code (or none): bit by bit, canonical order */
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)bits(z, 1);
        if (z->err) {
            return -1;
        }
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return fail(z, SLATE_INFLATE_BAD);
}

/* ---- blocks ---- */

static const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                 12, 13, 13};

static void stored(slate_inflate_t *z)
{
    bits(z, z->nbits & 7);   /* to the byte boundary */
    unsigned len = bits(z, 16), nlen = bits(z, 16);
    if (z->err) {
        return;
    }
    if (len != (~nlen & 0xFFFF)) {
        fail(z, SLATE_INFLATE_BAD);
        return;
    }
    while (len-- && !z->err) {
        emit(z, (uint8_t)bits(z, 8));
    }
}

static void codes(slate_inflate_t *z)
{
    while (!z->err) {
        int sym = decode(z, &z->lit);
        if (sym < 256) {
            if (sym < 0) {
                return;
            }
            emit(z, (uint8_t)sym);
            continue;
        }
        if (sym == 256) {
            return;
        }
        sym -= 257;
        if (sym >= 29) {
            fail(z, SLATE_INFLATE_BAD);
            return;
        }
        unsigned len = LBASE[sym] + bits(z, LEXT[sym]);
        int ds = decode(z, &z->dist);
        if (ds < 0) {
            return;
        }
        if (ds >= 30) {
            fail(z, SLATE_INFLATE_BAD);
            return;
        }
        unsigned d = DBASE[ds] + bits(z, DEXT[ds]);
        if (z->err) {
            return;
        }
        if (d > z->pos && !z->full) {
            fail(z, SLATE_INFLATE_BAD);   /* before the start of the output */
            return;
        }
        uint32_t from = (z->pos - d) & WMASK;
        while (len-- && !z->err) {
            emit(z, z->window[from]);
            from = (from + 1) & WMASK;
        }
    }
}

static void fixed(slate_inflate_t *z)
{
    uint8_t length[288];
    int s = 0;
    for (; s < 144; s++) length[s] = 8;
    for (; s < 256; s++) length[s] = 9;
    for (; s < 280; s++) length[s] = 7;
    for (; s < 288; s++) length[s] = 8;
    construct(&z->lit, length, 288);
    for (s = 0; s < 30; s++) length[s] = 5;
    construct(&z->dist, length, 30);
    codes(z);
}

static void dynamic(slate_inflate_t *z)
{
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t length[286 + 30];
    int nlen = (int)bits(z, 5) + 257, ndist = (int)bits(z, 5) + 1, ncode = (int)bits(z, 4) + 4;
    if (z->err) {
        return;
    }
    if (nlen > 286 || ndist > 30) {
        fail(z, SLATE_INFLATE_BAD);
        return;
    }
    memset(length, 0, 19);
    for (int i = 0; i < ncode; i++) {
        length[ORDER[i]] = (uint8_t)bits(z, 3);
    }
    if (z->err) {
        return;
    }
    if (construct(&z->lit, length, 19) != 0) {   /* the code-length code must be complete */
        fail(z, SLATE_INFLATE_BAD);
        return;
    }
    for (int i = 0; i < nlen + ndist;) {
        int sym = decode(z, &z->lit);
        if (sym < 0) {
            return;
        }
        if (sym < 16) {
            length[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t v = 0;
        int rep;
        if (sym == 16) {
            if (i == 0) {
                fail(z, SLATE_INFLATE_BAD);
                return;
            }
            v = length[i - 1];
            rep = 3 + (int)bits(z, 2);
        } else if (sym == 17) {
            rep = 3 + (int)bits(z, 3);
        } else {
            rep = 11 + (int)bits(z, 7);
        }
        if (z->err) {
            return;
        }
        if (i + rep > nlen + ndist) {
            fail(z, SLATE_INFLATE_BAD);
            return;
        }
        while (rep--) {
            length[i++] = v;
        }
    }
    if (length[256] == 0) {   /* no end-of-block code */
        fail(z, SLATE_INFLATE_BAD);
        return;
    }
    /* an incomplete code is allowed only when it is a single code of one bit */
    int err = construct(&z->lit, length, nlen);
    if (err < 0 || (err > 0 && nlen - z->lit.count[0] != 1)) {
        fail(z, SLATE_INFLATE_BAD);
        return;
    }
    err = construct(&z->dist, length + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - z->dist.count[0] != 1)) {
        fail(z, SLATE_INFLATE_BAD);
        return;
    }
    codes(z);
}

int slate_inflate_zlib(slate_inflate_t *z, int (*in)(void *ctx), bool (*out)(void *ctx, const uint8_t *p, size_t n),
                       void *ctx)
{
    z->in = in;
    z->out = out;
    z->ctx = ctx;
    z->bits = 0;
    z->nbits = 0;
    z->err = SLATE_INFLATE_OK;
    z->pos = 0;
    z->full = false;
    z->adler_a = 1;
    z->adler_b = 0;

    unsigned cmf = bits(z, 8), flg = bits(z, 8);
    if (z->err) {
        return z->err;
    }
    /* deflate, window up to 32 KB, check bits right, no preset dictionary */
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || (cmf << 8 | flg) % 31 != 0 || (flg & 0x20)) {
        return SLATE_INFLATE_BAD;
    }
    unsigned last;
    do {
        last = bits(z, 1);
        unsigned type = bits(z, 2);
        if (z->err) {
            break;
        }
        if (type == 0) {
            stored(z);
        } else if (type == 1) {
            fixed(z);
        } else if (type == 2) {
            dynamic(z);
        } else {
            fail(z, SLATE_INFLATE_BAD);
        }
    } while (!last && !z->err);
    flush(z);
    if (z->err) {
        return z->err;
    }
    bits(z, z->nbits & 7);
    uint32_t adler = 0;
    for (int i = 0; i < 4; i++) {
        adler = adler << 8 | bits(z, 8);
    }
    if (z->err) {
        return z->err;
    }
    if (adler != (z->adler_b << 16 | z->adler_a)) {
        return SLATE_INFLATE_BAD;
    }
    return SLATE_INFLATE_OK;
}

#endif
