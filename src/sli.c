/* .sli images: see sli.h for the format. slate_sli_open checks a whole file once; slate_sli_draw then walks the
   rows it is asked for without any check (it runs for every band of every frame that shows the image). */
#include "slate_speed.h"   /* first: the code below is built for speed */
#include <stdint.h>
#include <string.h>

#include "sli.h"

/* Colours are read as uint16_t in the CPU's order and the format is little-endian (as are the boards). */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "sli.c reads .sli colours in the CPU's byte order: little-endian targets only"
#endif

#define SLI_HEADER 16
#define SLI_MAX 32767
#define SLI_FLAG_HAS_ALPHA 1u
#define SLI_FLAG_OPAQUE 2u

enum { K_EMPTY = 0, K_REPEAT = 1, K_DIFFERENT = 2, K_SEMI = 3 };

/* Byte by byte: a repeated colour and the row table may sit at any offset (the board faults on unaligned loads). */
static inline unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

/* Checks the row at offset *o of the data d, which must end at or before end: its commands cover exactly w
   pixels. *o: where the commands end. *mixed: set when the row has empty or semi-transparent pixels. */
static bool check_row(const uint8_t *d, size_t *o, size_t end, unsigned w, bool *mixed)
{
    size_t p = *o;
    unsigned x = 0;
    while (x < w) {
        if (p >= end) {
            return false;
        }
        unsigned cmd = d[p++], kind = cmd >> 6, n = cmd & 63;
        if (n == 0) {
            if (end - p < 2) {
                return false;
            }
            n = rd16(d + p);
            p += 2;
            if (n < 64 || n > SLI_MAX) {
                return false;
            }
        }
        if (n > w - x) {
            return false;
        }
        if (kind == K_REPEAT) {
            if (end - p < 2) {
                return false;
            }
            p += 2;
        } else if (kind != K_EMPTY) {
            if (p & 1) {   /* the padding byte before the colours */
                if (p >= end || d[p] != 0) {
                    return false;
                }
                p++;
            }
            size_t need = (size_t)n * (kind == K_DIFFERENT ? 2 : 3);
            if (end - p < need) {
                return false;
            }
            p += need;
        }
        if (kind == K_EMPTY || kind == K_SEMI) {
            *mixed = true;
        }
        x += n;
    }
    *o = p;
    return true;
}

bool slate_sli_open(slate_sli_t *im, const uint8_t *data, size_t size)
{
    if (!data || size < SLI_HEADER || ((uintptr_t)data & 1) || memcmp(data, "SLI1", 4) != 0 || data[4] != 1) {
        return false;
    }
    unsigned flags = data[5], w = rd16(data + 6), h = rd16(data + 8);
    if ((flags != SLI_FLAG_HAS_ALPHA && flags != SLI_FLAG_OPAQUE) || w < 1 || w > SLI_MAX || h < 1 || h > SLI_MAX ||
        rd16(data + 10) != 0) {
        return false;
    }
    size_t base = SLI_HEADER + 4 * (size_t)h;
    uint32_t len = rd32(data + 12);
    if (size < base || size - base != len) {
        return false;
    }
    const uint8_t *tab = data + SLI_HEADER, *d = data + base;
    size_t end = 0;   /* where the previous row's commands end */
    bool mixed = false;
    for (unsigned j = 0; j < h; j++) {
        size_t off = rd32(tab + 4 * (size_t)j);
        /* rows in order, each at the first multiple of 4 after the previous one, zero bytes in between */
        if (off != ((end + 3) & ~(size_t)3) || off > len) {
            return false;
        }
        for (; end < off; end++) {
            if (d[end] != 0) {
                return false;
            }
        }
        size_t limit = j + 1 < h ? rd32(tab + 4 * (size_t)(j + 1)) : len;
        if (limit < off || limit > len || !check_row(d, &end, limit, w, &mixed)) {
            return false;
        }
    }
    if (end != len || mixed != (flags == SLI_FLAG_HAS_ALPHA)) {   /* no bytes after the last row; flag as pixels */
        return false;
    }
    im->data = data;
    im->size = size;
    im->w = (uint16_t)w;
    im->h = (uint16_t)h;
    im->opaque = flags == SLI_FLAG_OPAQUE;
    return true;
}

/* The alpha row of opaque colours drawn with an opacity below 255 (copy_a), taken a piece at a time. */
#define ONES 64
#define O8 255, 255, 255, 255, 255, 255, 255, 255
static const uint8_t ones[ONES] = { O8, O8, O8, O8, O8, O8, O8, O8 };

void slate_sli_draw(slate_buf_t *b, const slate_sli_t *im, int x, int y, uint8_t a, slate_rect_t clip)
{
    slate_sli_draw_ops(slate_gfx(), b, im, x, y, a, clip);
}

void slate_sli_draw_ops(const slate_gfx_ops_t *g, slate_buf_t *b, const slate_sli_t *im, int x, int y, uint8_t a,
                        slate_rect_t clip)
{
    if (a == 0) {
        return;
    }
    /* the window: clip, buffer and image (in 64 bits: x + w must not overflow) */
    int64_t wx1 = (int64_t)x + im->w, wy1 = (int64_t)y + im->h;
    int bx1 = imin(clip.x1, b->w), by1 = imin(clip.y1, b->h);
    int cx0 = imax(imax(clip.x0, 0), x), cy0 = imax(imax(clip.y0, 0), y);
    int cx1 = wx1 < bx1 ? (int)wx1 : bx1, cy1 = wy1 < by1 ? (int)wy1 : by1;
    if (cx0 >= cx1 || cy0 >= cy1) {
        return;
    }
    const uint8_t *tab = im->data + SLI_HEADER, *d = tab + 4 * (size_t)im->h;
    for (int Y = cy0; Y < cy1; Y++) {
        const uint8_t *p = d + rd32(tab + 4 * (size_t)(Y - y));
        const slate_rect_t rc = { cx0, Y, cx1, Y + 1 };
        int px = x;   /* buffer column of the command's first pixel; x < cx1 and px + n <= x + w, so no overflow */
        while (px < cx1) {
            unsigned cmd = *p++, n = cmd & 63;
            if (n == 0) {
                n = rd16(p);
                p += 2;
            }
            int e = px + (int)n;
            switch (cmd >> 6) {
            case K_EMPTY:
                break;
            case K_REPEAT: {
                uint16_t c = (uint16_t)rd16(p);   /* any offset: byte by byte */
                p += 2;
                if (e > cx0) {
                    slate_rect_t r = { imax(px, cx0), Y, imin(e, cx1), Y + 1 };
                    if (a == 255) {
                        g->fill(b, r, c);
                    } else {
                        g->fill_a(b, r, c, a);
                    }
                }
                break;
            }
            case K_DIFFERENT: {
                p += (uintptr_t)p & 1;   /* even from the data start, which is at an even address (open) */
                const uint16_t *s = (const uint16_t *)(const void *)p;
                if (e > cx0) {
                    if (a == 255) {
                        g->copy(b, px, Y, s, (int)n, 1, (int)n, rc);
                    } else {
                        for (int i = imax(px, cx0) - px, i1 = imin(e, cx1) - px; i < i1; i += ONES) {
                            int m = imin(ONES, i1 - i);
                            g->copy_a(b, px + i, Y, s + i, ones, m, 1, m, a, rc);
                        }
                    }
                }
                p += 2 * (size_t)n;
                break;
            }
            default: {   /* K_SEMI */
                p += (uintptr_t)p & 1;
                if (e > cx0) {
                    g->copy_a(b, px, Y, (const uint16_t *)(const void *)p, p + 2 * (size_t)n, (int)n, 1, (int)n, a,
                              rc);
                }
                p += 3 * (size_t)n;
                break;
            }
            }
            px = e;
        }
    }
}
