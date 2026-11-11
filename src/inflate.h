/* inflate.h - a small streaming zlib/deflate decoder (RFC 1950, RFC 1951) for the PNG decoder. Pure C, no ESP-IDF;
   written for Slate (no third-party code). Built only with SLATE_HAS_PNG.

   The compressed bytes are pulled one at a time from in(); the output goes through a 32 KB window (the largest
   distance deflate uses) and is handed to out() in pieces of up to 32 KB as the window fills and at the end. The
   whole state, window included, is one slate_inflate_t (about 35 KB): allocate it, no other memory is used. */
#ifndef SLATE_INFLATE_H
#define SLATE_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLATE_INFLATE_WINDOW 32768u

/* A Huffman code: a direct table of the codes up to FAST bits, and the canonical counts and symbols for the rest. */
#define SLATE_INFLATE_FAST 9
typedef struct {
    uint16_t fast[1 << SLATE_INFLATE_FAST];   /* by the next FAST bits: length << 9 | symbol; 0: a longer code */
    uint16_t count[16];                       /* codes of each length */
    uint16_t symbol[288];                     /* symbols by code */
} slate_inflate_code_t;

typedef struct {
    int (*in)(void *ctx);                                   /* the next byte 0..255, < 0: no more */
    bool (*out)(void *ctx, const uint8_t *p, size_t n);     /* false: stop */
    void *ctx;
    uint32_t bits;      /* bit buffer */
    int nbits;
    int err;            /* SLATE_INFLATE_* once something went wrong */
    uint32_t pos;       /* next write in the window */
    bool full;          /* the window wrapped at least once: every distance up to 32768 is valid */
    uint32_t adler_a, adler_b;
    slate_inflate_code_t lit, dist;
    uint8_t window[SLATE_INFLATE_WINDOW];
} slate_inflate_t;

enum {
    SLATE_INFLATE_OK = 0,
    SLATE_INFLATE_BAD,      /* not a valid zlib stream (header, block, code, distance, Adler-32) */
    SLATE_INFLATE_INPUT,    /* in() had no more bytes */
    SLATE_INFLATE_OUTPUT,   /* out() asked to stop */
};

/* Decodes one zlib stream (header, deflate blocks, Adler-32) to the end; the bytes after it are not read.
   Returns SLATE_INFLATE_OK or the first error. */
int slate_inflate_zlib(slate_inflate_t *z, int (*in)(void *ctx), bool (*out)(void *ctx, const uint8_t *p, size_t n),
                       void *ctx);

#endif
