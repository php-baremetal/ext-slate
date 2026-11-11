/* png.c - PNG to .sli on the board (png.h): chunks read in a stream, the image data inflated through a 32 KB
   window, each row unfiltered, turned into RGBA8 and given to the .sli encoder. */
#include "slate_speed.h"   /* first: the row loops are built for speed */
#include "slate_config.h"

#if SLATE_HAS_PNG

#include <string.h>

#include "inflate.h"
#include "png.h"
#include "sli_enc.h"

#define PNG_MAX 32767

typedef struct {
    int (*read)(void *ctx, uint8_t *buf, size_t n);
    void *rctx;
    void *(*alloc)(size_t);
    void (*release)(void *);
    slate_img_err_t err;   /* the first error */

    /* input */
    uint8_t buf[512];
    size_t at, end;

    /* the chunk being read */
    uint32_t left;   /* bytes of its data not read yet */
    uint32_t crc;
    char type[4];
    bool idat;       /* it is an IDAT chunk */

    /* the image */
    int w, h, ctype, depth, channels;
    bool have_ihdr, have_plte, started, done;
    size_t lb;    /* bytes of a row, without the filter byte */
    size_t bpp;   /* bytes per pixel for the filters (at least 1) */
    int npal;
    int ntrns;    /* -1: no tRNS */
    uint8_t pal[256 * 4];   /* RGBA of each palette entry, once the image data starts */
    uint8_t trns[256];

    /* rows */
    uint8_t *cur, *prev;   /* filter byte + lb bytes each */
    uint8_t *rgba;         /* w * 4 bytes, NULL for 8-bit RGBA (the row itself is given) */
    size_t fill;           /* bytes of cur filled */
    int y;
    slate_sli_enc_t *enc;
} png_t;

static bool set_err(png_t *p, slate_img_err_t err)
{
    if (p->err == SLATE_IMG_OK) {
        p->err = err;
    }
    return false;
}

/* ---- CRC-32 (ISO 3309, as PNG), four bits at a time ---- */

static const uint32_t CRC16[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

static inline uint32_t crc_byte(uint32_t crc, uint8_t b)
{
    crc ^= b;
    crc = (crc >> 4) ^ CRC16[crc & 15];
    return (crc >> 4) ^ CRC16[crc & 15];
}

/* ---- input ---- */

/* The next byte of the file; -1 (with an error) when the file ends or cannot be read. */
static int raw_byte(png_t *p)
{
    if (p->at == p->end) {
        if (p->err) {
            return -1;
        }
        int n = p->read(p->rctx, p->buf, sizeof p->buf);
        if (n < 0 || n > (int)sizeof p->buf) {
            set_err(p, SLATE_IMG_UNREADABLE);
            return -1;
        }
        if (n == 0) {
            set_err(p, SLATE_IMG_BAD_FORMAT);   /* the file ends early */
            return -1;
        }
        p->at = 0;
        p->end = (size_t)n;
    }
    return p->buf[p->at++];
}

static uint32_t raw_u32(png_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = v << 8 | (uint8_t)raw_byte(p);
    }
    return v;
}

/* The next byte of the chunk's data (the caller checks left). */
static inline int data_byte(png_t *p)
{
    int c = raw_byte(p);
    if (c < 0) {
        return -1;
    }
    p->left--;
    p->crc = crc_byte(p->crc, (uint8_t)c);
    return c;
}

static bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/* Reads the length and type of the next chunk. */
static bool chunk_start(png_t *p)
{
    uint32_t len = raw_u32(p);
    p->crc = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) {
        int c = raw_byte(p);
        p->type[i] = (char)c;
        p->crc = crc_byte(p->crc, (uint8_t)c);
    }
    if (p->err) {
        return false;
    }
    if (len > 0x7FFFFFFFu || !is_letter(p->type[0]) || !is_letter(p->type[1]) || !is_letter(p->type[2]) ||
        !is_letter(p->type[3])) {
        return set_err(p, SLATE_IMG_BAD_FORMAT);
    }
    p->left = len;
    return true;
}

/* Reads the rest of the chunk's data into out (up to cap bytes kept) and checks its CRC. */
static bool chunk_end(png_t *p, uint8_t *out, size_t cap)
{
    for (size_t i = 0; p->left; i++) {
        int c = data_byte(p);
        if (c < 0) {
            return false;
        }
        if (i < cap) {
            out[i] = (uint8_t)c;
        }
    }
    uint32_t want = raw_u32(p);
    if (p->err) {
        return false;
    }
    if (want != (p->crc ^ 0xFFFFFFFFu)) {
        return set_err(p, SLATE_IMG_BAD_FORMAT);
    }
    return true;
}

static bool is_type(const png_t *p, const char *t) { return memcmp(p->type, t, 4) == 0; }

/* ---- rows ---- */

static bool unfilter(png_t *p)
{
    uint8_t *c = p->cur + 1;
    const uint8_t *u = p->prev + 1;
    size_t n = p->lb, bpp = p->bpp;
    switch (p->cur[0]) {
    case 0:
        break;
    case 1:
        for (size_t i = bpp; i < n; i++) c[i] = (uint8_t)(c[i] + c[i - bpp]);
        break;
    case 2:
        for (size_t i = 0; i < n; i++) c[i] = (uint8_t)(c[i] + u[i]);
        break;
    case 3:
        for (size_t i = 0; i < n; i++) c[i] = (uint8_t)(c[i] + (((i >= bpp ? c[i - bpp] : 0) + u[i]) >> 1));
        break;
    case 4:
        for (size_t i = 0; i < n; i++) {
            int a = i >= bpp ? c[i - bpp] : 0, b = u[i], cc = i >= bpp ? u[i - bpp] : 0;
            int pp = a + b - cc, pa = pp > a ? pp - a : a - pp, pb = pp > b ? pp - b : b - pp,
                pc = pp > cc ? pp - cc : cc - pp;
            c[i] = (uint8_t)(c[i] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : cc));
        }
        break;
    default:
        return set_err(p, SLATE_IMG_BAD_FORMAT);   /* unknown filter type */
    }
    return true;
}

/* Sample k of the unfiltered row at the image's bit depth (1, 2, 4 or 8). */
static inline unsigned sample(const uint8_t *c, size_t k, int depth)
{
    if (depth == 8) {
        return c[k];
    }
    size_t bit = k * (size_t)depth;
    return (unsigned)(c[bit >> 3] >> (8 - depth - (int)(bit & 7))) & ((1u << depth) - 1);
}

/* The unfiltered row as RGBA8, the way phpbm's PngReader gives it. */
static bool to_rgba(png_t *p)
{
    const uint8_t *c = p->cur + 1;
    uint8_t *o = p->rgba;
    int w = p->w, depth = p->depth;
    bool key = p->ntrns >= 0;
    switch (p->ctype) {
    case 0: {
        unsigned max = (1u << depth) - 1, k = key ? (unsigned)(p->trns[0] << 8 | p->trns[1]) : 0x10000;
        for (int x = 0; x < w; x++, o += 4) {
            unsigned v = sample(c, (size_t)x, depth);
            o[0] = o[1] = o[2] = (uint8_t)(v * 255 / max);
            o[3] = v == k ? 0 : 255;
        }
        break;
    }
    case 2: {
        unsigned kr = 0x10000, kg = 0, kb = 0;
        if (key) {
            kr = (unsigned)(p->trns[0] << 8 | p->trns[1]);
            kg = (unsigned)(p->trns[2] << 8 | p->trns[3]);
            kb = (unsigned)(p->trns[4] << 8 | p->trns[5]);
        }
        for (int x = 0; x < w; x++, o += 4, c += 3) {
            o[0] = c[0];
            o[1] = c[1];
            o[2] = c[2];
            o[3] = c[0] == kr && c[1] == kg && c[2] == kb ? 0 : 255;
        }
        break;
    }
    case 3:
        for (int x = 0; x < w; x++, o += 4) {
            unsigned i = sample(c, (size_t)x, depth);
            if ((int)i >= p->npal) {
                return set_err(p, SLATE_IMG_BAD_FORMAT);   /* palette index out of range */
            }
            memcpy(o, p->pal + 4 * i, 4);
        }
        break;
    case 4:
        for (int x = 0; x < w; x++, o += 4, c += 2) {
            o[0] = o[1] = o[2] = c[0];
            o[3] = c[1];
        }
        break;
    }
    return true;
}

static bool row_done(png_t *p)
{
    if (!unfilter(p)) {
        return false;
    }
    const uint8_t *rgba = p->cur + 1;
    if (p->rgba) {
        if (!to_rgba(p)) {
            return false;
        }
        rgba = p->rgba;
    }
    if (!slate_sli_enc_row(p->enc, rgba)) {
        return set_err(p, SLATE_IMG_NO_MEMORY);
    }
    uint8_t *t = p->cur;
    p->cur = p->prev;
    p->prev = t;
    p->y++;
    return true;
}

/* inflate's output: the image data, cut into rows. */
static bool sink(void *ctx, const uint8_t *d, size_t n)
{
    png_t *p = ctx;
    while (n) {
        if (p->y == p->h) {
            return set_err(p, SLATE_IMG_BAD_FORMAT);   /* image data too long */
        }
        size_t take = p->lb + 1 - p->fill;
        if (take > n) {
            take = n;
        }
        memcpy(p->cur + p->fill, d, take);
        p->fill += take;
        d += take;
        n -= take;
        if (p->fill == p->lb + 1) {
            p->fill = 0;
            if (!row_done(p)) {
                return false;
            }
        }
    }
    return true;
}

/* inflate's input: the data of this IDAT chunk and of the next ones (ancillary chunks between them skipped). */
static int source(void *ctx)
{
    png_t *p = ctx;
    while (!p->left || !p->idat) {
        if (p->err || !chunk_end(p, NULL, 0) || !chunk_start(p)) {
            return -1;
        }
        p->idat = is_type(p, "IDAT");
        if (!p->idat && !(p->type[0] & 0x20)) {
            set_err(p, SLATE_IMG_BAD_FORMAT);   /* a critical chunk: the image data ends early */
            return -1;
        }
    }
    return data_byte(p);
}

/* The palette and tRNS checked and the row buffers and encoder made, at the first IDAT chunk. */
static bool start(png_t *p)
{
    if (p->ctype == 3) {
        if (!p->have_plte) {
            return set_err(p, SLATE_IMG_BAD_FORMAT);
        }
        if (p->ntrns > p->npal) {
            return set_err(p, SLATE_IMG_BAD_FORMAT);
        }
        for (int i = 0; i < p->npal; i++) {
            p->pal[4 * i + 3] = i < p->ntrns ? p->trns[i] : 255;
        }
    } else if (p->ctype == 0 || p->ctype == 2) {
        if (p->ntrns >= 0 && p->ntrns != (p->ctype == 0 ? 2 : 6)) {
            return set_err(p, SLATE_IMG_BAD_FORMAT);
        }
    } else {
        p->ntrns = -1;   /* ignored with an alpha channel */
    }
    bool direct = p->ctype == 6;
    size_t rows = 2 * (p->lb + 1) + (direct ? 0 : 4 * (size_t)p->w);
    p->cur = p->alloc(rows);
    if (!p->cur) {
        return set_err(p, SLATE_IMG_NO_MEMORY);
    }
    p->prev = p->cur + p->lb + 1;
    memset(p->prev, 0, p->lb + 1);
    p->rgba = direct ? NULL : p->prev + p->lb + 1;
    p->enc = slate_sli_enc_new(p->w, p->h, p->alloc, p->release);
    if (!p->enc) {
        return set_err(p, SLATE_IMG_NO_MEMORY);
    }
    p->started = true;
    return true;
}

/* The image data, from the IDAT chunk just started to the end of the zlib stream. */
static bool image_data(png_t *p)
{
    slate_inflate_t *z = p->alloc(sizeof *z);
    if (!z) {
        return set_err(p, SLATE_IMG_NO_MEMORY);
    }
    int r = slate_inflate_zlib(z, source, sink, p);
    p->release(z);
    if (r != SLATE_INFLATE_OK || p->y != p->h || p->fill) {
        return set_err(p, SLATE_IMG_BAD_FORMAT);   /* (unless read, memory or a row failed first) */
    }
    p->done = true;
    /* the rows are not needed any more: their memory goes back before the file is put together */
    p->release(p->cur < p->prev ? p->cur : p->prev);
    p->cur = p->prev = p->rgba = NULL;
    return true;
}

static bool header(png_t *p, const uint8_t *d)
{
    uint32_t w = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    uint32_t h = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];
    int depth = d[8], ctype = d[9];
    if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu || d[10] != 0 || d[11] != 0 || d[12] > 1) {
        return set_err(p, SLATE_IMG_BAD_FORMAT);
    }
    bool valid;
    switch (ctype) {
    case 0: valid = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
    case 3: valid = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
    case 2: case 4: case 6: valid = depth == 8 || depth == 16; break;
    default: valid = false;
    }
    if (!valid) {
        return set_err(p, SLATE_IMG_BAD_FORMAT);
    }
    if (w > PNG_MAX || h > PNG_MAX || depth == 16 || d[12] == 1) {
        return set_err(p, SLATE_IMG_UNSUPPORTED);
    }
    static const int CHANNELS[7] = {1, 0, 3, 1, 2, 0, 4};
    p->w = (int)w;
    p->h = (int)h;
    p->depth = depth;
    p->ctype = ctype;
    p->channels = CHANNELS[ctype];
    p->lb = ((size_t)p->w * (size_t)p->channels * (size_t)depth + 7) / 8;
    p->bpp = (size_t)p->channels * (size_t)depth / 8;
    if (p->bpp < 1) {
        p->bpp = 1;
    }
    p->have_ihdr = true;
    return true;
}

static void decode(png_t *p)
{
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    for (int i = 0; i < 8; i++) {
        int c = raw_byte(p);
        if (c < 0) {
            return;
        }
        if (c != SIG[i]) {
            set_err(p, SLATE_IMG_BAD_FORMAT);
            return;
        }
    }
    for (;;) {
        if (!chunk_start(p)) {
            return;
        }
        if (!p->have_ihdr && !is_type(p, "IHDR")) {
            set_err(p, SLATE_IMG_BAD_FORMAT);   /* IHDR must come first */
            return;
        }
        uint32_t len = p->left;
        if (is_type(p, "IHDR")) {
            uint8_t d[13];
            if (!chunk_end(p, d, sizeof d)) {
                return;
            }
            if (p->have_ihdr || len != 13) {
                set_err(p, SLATE_IMG_BAD_FORMAT);
                return;
            }
            if (!header(p, d)) {
                return;
            }
        } else if (is_type(p, "PLTE")) {
            uint8_t d[768];
            if (!chunk_end(p, d, sizeof d)) {
                return;
            }
            if (len == 0 || len % 3 || len > 768) {
                set_err(p, SLATE_IMG_BAD_FORMAT);
                return;
            }
            if (!p->started) {   /* (a PLTE after the image data cannot change it) */
                p->npal = (int)(len / 3);
                for (int i = 0; i < p->npal; i++) {
                    memcpy(p->pal + 4 * i, d + 3 * i, 3);
                }
                p->have_plte = true;
            }
        } else if (is_type(p, "tRNS")) {
            if (!chunk_end(p, p->trns, sizeof p->trns)) {
                return;
            }
            if (!p->started) {
                /* longer than 256 bytes: too long for any image that uses it (ignored in the others) */
                p->ntrns = len > 256 ? 257 : (int)len;
            }
        } else if (is_type(p, "IDAT")) {
            if (p->done) {
                if (!chunk_end(p, NULL, 0)) {   /* after the end of the zlib stream: ignored */
                    return;
                }
                continue;
            }
            p->idat = true;
            if (!start(p) || !image_data(p)) {
                return;
            }
            if (!chunk_end(p, NULL, 0)) {   /* the rest of the last IDAT chunk */
                return;
            }
        } else if (is_type(p, "IEND")) {
            if (!chunk_end(p, NULL, 0)) {
                return;
            }
            if (!p->done) {
                set_err(p, SLATE_IMG_BAD_FORMAT);   /* no image data */
            }
            return;
        } else {
            if (!chunk_end(p, NULL, 0)) {
                return;
            }
            if (!(p->type[0] & 0x20)) {
                set_err(p, SLATE_IMG_UNSUPPORTED);   /* an unknown critical chunk */
                return;
            }
        }
    }
}

slate_img_err_t slate_png_to_sli(int (*read)(void *ctx, uint8_t *buf, size_t n), void *ctx,
                                 void *(*alloc)(size_t), void (*release)(void *),
                                 uint8_t **sli, size_t *size)
{
    *sli = NULL;
    *size = 0;
    png_t *p = alloc(sizeof *p);
    if (!p) {
        return SLATE_IMG_NO_MEMORY;
    }
    memset(p, 0, sizeof *p);
    p->read = read;
    p->rctx = ctx;
    p->alloc = alloc;
    p->release = release;
    p->ntrns = -1;
    decode(p);
    if (p->cur) {
        release(p->cur < p->prev ? p->cur : p->prev);
    }
    slate_img_err_t err = p->err;
    if (p->enc) {
        if (err != SLATE_IMG_OK) {
            slate_sli_enc_abort(p->enc);
        } else {
            *sli = slate_sli_enc_finish(p->enc, size);
            if (!*sli) {
                err = SLATE_IMG_NO_MEMORY;
            }
        }
    }
    release(p);
    return err;
}

#endif
