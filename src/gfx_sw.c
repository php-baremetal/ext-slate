/* Software graphics layer of Slate: see slate_gfx.h for the pixel format and the blend formula. */
#include "slate_speed.h"   /* first: the code below is built for speed */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "slate_config.h"
#include "slate_gfx.h"
#include "slate_sw.h"

static inline unsigned div255(unsigned x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* Blends the colour s over d, a = 0..255. */
static inline uint16_t blend565(uint16_t d, uint16_t s, unsigned a)
{
    unsigned ia = 255 - a;
    unsigned r = div255(((s >> 11) & 31) * a + ((d >> 11) & 31) * ia);
    unsigned g = div255(((s >> 5) & 63) * a + ((d >> 5) & 63) * ia);
    unsigned b = div255((s & 31) * a + (d & 31) * ia);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

typedef uint32_t __attribute__((__may_alias__)) u32a;

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

static slate_rect_t clip_buf(const slate_buf_t *b, slate_rect_t r)
{
    r.x0 = imax(r.x0, 0);
    r.y0 = imax(r.y0, 0);
    r.x1 = imin(r.x1, b->w);
    r.y1 = imin(r.y1, b->h);
    return r;
}

uint16_t slate_rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

void slate_swap565_row(uint16_t *dst, const uint16_t *src, int n)
{
    int i = 0;
    if ((((uintptr_t)dst | (uintptr_t)src) & 3) == 0) {   /* two pixels per 32-bit word, four words a turn */
        const uint32_t m = 0x00FF00FFu;
        for (; i + 7 < n; i += 8) {
            uint32_t v0 = *(const u32a *)(src + i), v1 = *(const u32a *)(src + i + 2);
            uint32_t v2 = *(const u32a *)(src + i + 4), v3 = *(const u32a *)(src + i + 6);
            *(u32a *)(dst + i) = ((v0 & m) << 8) | ((v0 >> 8) & m);
            *(u32a *)(dst + i + 2) = ((v1 & m) << 8) | ((v1 >> 8) & m);
            *(u32a *)(dst + i + 4) = ((v2 & m) << 8) | ((v2 >> 8) & m);
            *(u32a *)(dst + i + 6) = ((v3 & m) << 8) | ((v3 >> 8) & m);
        }
        for (; i + 1 < n; i += 2) {
            uint32_t v = *(const u32a *)(src + i);
            *(u32a *)(dst + i) = ((v & m) << 8) | ((v >> 8) & m);
        }
    }
    for (; i < n; i++) {
        dst[i] = slate_swap565(src[i]);
    }
}

static void sw_fill(slate_buf_t *b, slate_rect_t r, uint16_t c)
{
    r = clip_buf(b, r);
    uint32_t pair = (uint32_t)c | ((uint32_t)c << 16);
    for (int y = r.y0; y < r.y1; y++) {
        uint16_t *row = b->px + (size_t)y * b->stride;
        int x = r.x0;
        if (x < r.x1 && ((uintptr_t)(row + x) & 2)) {
            row[x++] = c;
        }
        for (; x + 1 < r.x1; x += 2) {
            *(u32a *)(row + x) = pair;
        }
        if (x < r.x1) {
            row[x] = c;
        }
    }
}

/* blend565 with one colour and alpha, as three tables: for each value of a channel of the destination, the
   channel of the result, already in place. Kept between calls: a fill comes row by row with the same colour. One
   compositor at a time draws (slate_gfx.h), so one set serves. */
static struct {
    bool ok;
    uint16_t c;
    uint8_t a;
    uint16_t r[32], g[64], b[32];
} fa_lut;

static void sw_fill_a(slate_buf_t *b, slate_rect_t r, uint16_t c, uint8_t a)
{
    if (a == 0) {
        return;
    }
    if (a == 255) {
        sw_fill(b, r, c);
        return;
    }
    r = clip_buf(b, r);
    if (r.x0 >= r.x1 || r.y0 >= r.y1) {
        return;
    }
    if (!fa_lut.ok || fa_lut.c != c || fa_lut.a != a) {
        if ((long)(r.x1 - r.x0) * (r.y1 - r.y0) < 128) {   /* too few pixels to pay for the tables */
            for (int y = r.y0; y < r.y1; y++) {
                uint16_t *row = b->px + (size_t)y * b->stride;
                for (int x = r.x0; x < r.x1; x++) {
                    row[x] = blend565(row[x], c, a);
                }
            }
            return;
        }
        unsigned ia = 255 - a, sr = ((c >> 11) & 31) * a, sg = ((c >> 5) & 63) * a, sb = (c & 31) * a;
        for (unsigned v = 0; v < 64; v++) {
            fa_lut.g[v] = (uint16_t)(div255(sg + v * ia) << 5);
        }
        for (unsigned v = 0; v < 32; v++) {
            fa_lut.r[v] = (uint16_t)(div255(sr + v * ia) << 11);
            fa_lut.b[v] = (uint16_t)div255(sb + v * ia);
        }
        fa_lut.c = c;
        fa_lut.a = a;
        fa_lut.ok = true;
    }
    const uint16_t *tr = fa_lut.r, *tg = fa_lut.g, *tb = fa_lut.b;
    for (int y = r.y0; y < r.y1; y++) {
        uint16_t *row = b->px + (size_t)y * b->stride;
        for (int x = r.x0; x < r.x1; x++) {
            unsigned d = row[x];
            row[x] = (uint16_t)(tr[d >> 11] | tg[(d >> 5) & 63] | tb[d & 31]);
        }
    }
}

static void sw_fill_mask(slate_buf_t *b, slate_rect_t r, uint16_t c, uint8_t a, const uint8_t *cov, int cov_stride)
{
    if (a == 0) {
        return;
    }
    slate_rect_t cr = clip_buf(b, r);
    for (int y = cr.y0; y < cr.y1; y++) {
        uint16_t *row = b->px + (size_t)y * b->stride;
        const uint8_t *m = cov + (size_t)(y - r.y0) * cov_stride;   /* m[x - r.x0] */
        if (a == 255) {
            for (int x = cr.x0; x < cr.x1; x++) {
                unsigned ea = m[x - r.x0];
                if (ea != 0) {
                    row[x] = ea == 255 ? c : blend565(row[x], c, ea);
                }
            }
        } else {
            /* blend565 with red and blue side by side in one word (each sum stays below 2^16, so the two halves
               do not touch and div255 works on both at once): the same values, fewer multiplies */
            const uint32_t srb = ((uint32_t)((c >> 11) & 31) << 16) | (c & 31), sg = (c >> 5) & 63;
            for (int x = cr.x0; x < cr.x1; x++) {
                unsigned ea = div255((unsigned)m[x - r.x0] * a);
                if (ea != 0) {
                    unsigned ia = 255 - ea, d = row[x];
                    uint32_t rb = srb * ea + ((((uint32_t)d >> 11) << 16) | (d & 31)) * ia + 0x00800080u;
                    rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
                    uint32_t g = sg * ea + ((d >> 5) & 63) * ia + 128u;
                    g = (g + (g >> 8)) >> 8;
                    row[x] = (uint16_t)(((rb >> 5) & 0xF800u) | (g << 5) | (rb & 31));
                }
            }
        }
    }
}

/* The part of an sw x sh image at (x, y) that lies inside clip and the buffer. */
static int copy_window(const slate_buf_t *b, int x, int y, int sw, int sh, slate_rect_t clip, slate_rect_t *out)
{
    slate_rect_t r = { x, y, x + sw, y + sh };
    r.x0 = imax(r.x0, clip.x0);
    r.y0 = imax(r.y0, clip.y0);
    r.x1 = imin(r.x1, clip.x1);
    r.y1 = imin(r.y1, clip.y1);
    r = clip_buf(b, r);
    *out = r;
    return r.x0 < r.x1 && r.y0 < r.y1;
}

static void sw_copy(slate_buf_t *b, int x, int y, const uint16_t *src, int sw, int sh, int sstride, slate_rect_t clip)
{
    slate_rect_t r;
    if (!copy_window(b, x, y, sw, sh, clip, &r)) {
        return;
    }
    for (int j = r.y0; j < r.y1; j++) {
        memmove(b->px + (size_t)j * b->stride + r.x0, src + (size_t)(j - y) * sstride + (r.x0 - x),
                (size_t)(r.x1 - r.x0) * 2);
    }
}

static void sw_copy_a(slate_buf_t *b, int x, int y, const uint16_t *src, const uint8_t *alpha, int sw, int sh,
                      int sstride, uint8_t a, slate_rect_t clip)
{
    slate_rect_t r;
    if (a == 0 || !copy_window(b, x, y, sw, sh, clip, &r)) {
        return;
    }
    for (int j = r.y0; j < r.y1; j++) {
        uint16_t *row = b->px + (size_t)j * b->stride;
        const uint16_t *s = src + (size_t)(j - y) * sstride;   /* s[i - x] */
        const uint8_t *al = alpha + (size_t)(j - y) * sstride;
        for (int i = r.x0; i < r.x1; i++) {
            unsigned pa = al[i - x];
            if (pa == 0) {
                continue;
            }
            if (pa == 255 && a == 255) {
                row[i] = s[i - x];
                continue;
            }
            unsigned A = a == 255 ? pa : div255(pa * a);
            unsigned ia = 255 - A;
            uint16_t sc = s[i - x], dc = row[i];
            unsigned sr = (sc >> 11) & 31, sg = (sc >> 5) & 63, sb = sc & 31;
            unsigned r5, g6, b5;
            if (a == 255) {
                r5 = sr + div255(((dc >> 11) & 31) * ia);
                g6 = sg + div255(((dc >> 5) & 63) * ia);
                b5 = sb + div255((dc & 31) * ia);
            } else {
                r5 = div255(sr * a + ((dc >> 11) & 31) * ia);
                g6 = div255(sg * a + ((dc >> 5) & 63) * ia);
                b5 = div255(sb * a + (dc & 31) * ia);
            }
            r5 = (unsigned)imin((int)r5, 31);
            g6 = (unsigned)imin((int)g6, 63);
            b5 = (unsigned)imin((int)b5, 31);
            row[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

/* ---- gradient ---- */

#if SLATE_HAS_GRADIENTS

/* t is handled in 40.24 fixed point (int64): the step dt keeps 24 fractional bits, so t = t0 + X * dt stays
   exact to about 1e-4 across thousands of columns. */
#define FX_ONE 16777216.0f            /* 2^24 */
#define FX_T_MAX 68719476736.0f       /* |t0|, |at| <= 4096: keeps (t - at) * 2^16 * 255 inside int64 */
#define FX_D_MAX 2147483648.0f        /* |dt| <= 128 per pixel: keeps X * dt inside int64 */

typedef struct { int64_t at; uint8_t c[4]; } fx_stop_t;   /* at in 40.24; c = A, R, G, B, colours premultiplied */

/* float -> int64 truncated toward zero, as the C conversion, without its library call: below 2^31 in magnitude the
   hardware conversion does it; above, a float is an integer (24-bit mantissa) and is rebuilt from its bits. v is
   finite and below 2^62 in magnitude. */
static inline int64_t f_to_i64(float v)
{
    if (v > -2147483648.0f && v < 2147483648.0f) {
        return (int32_t)v;
    }
    union { float f; uint32_t u; } bits = { v };
    int e = (int)((bits.u >> 23) & 255) - 150;   /* |v| = m * 2^e, e >= 8 here */
    int64_t m = (int64_t)((bits.u & 0x7FFFFFu) | 0x800000u) << e;
    return (bits.u >> 31) ? -m : m;
}

static inline int64_t to_fx(float v, float max)
{
    float s = v * FX_ONE;
    if (!(s > -max)) {   /* also NaN */
        s = -max;
    } else if (s > max) {
        s = max;
    }
    return f_to_i64(s >= 0 ? s + 0.5f : s - 0.5f);
}

/* 4x4 ordered (Bayer) dither thresholds, 0..15. */
static const uint8_t bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* One gradient pixel: premultiplied colour (pr, pg, pb) with alpha A over *p, times cov; the colour is dithered
   (threshold th, 0..15) when reduced to 5/6/5. */
static inline void put_pm(uint16_t *p, unsigned A, unsigned pr, unsigned pg, unsigned pb, unsigned cov, unsigned th)
{
    if (cov != 255) {
        A = div255(A * cov);
        pr = div255(pr * cov);
        pg = div255(pg * cov);
        pb = div255(pb * cov);
    }
    if (A == 0) {
        return;
    }
    unsigned r5 = imin((int)((pr + (th >> 1)) >> 3), 31);
    unsigned g6 = imin((int)((pg + (th >> 2)) >> 2), 63);
    unsigned b5 = imin((int)((pb + (th >> 1)) >> 3), 31);
    if (A != 255) {
        unsigned ia = 255 - A, d = *p;
        r5 = (unsigned)imin((int)(r5 + div255(((d >> 11) & 31) * ia)), 31);
        g6 = (unsigned)imin((int)(g6 + div255(((d >> 5) & 63) * ia)), 63);
        b5 = (unsigned)imin((int)(b5 + div255((d & 31) * ia)), 31);
    }
    *p = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* put_pm of an opaque pixel (A = 255, cov = 255), which does not depend on what is under it. */
static inline uint16_t pm_opaque(unsigned pr, unsigned pg, unsigned pb, unsigned th)
{
    unsigned r5 = (unsigned)imin((int)((pr + (th >> 1)) >> 3), 31);
    unsigned g6 = (unsigned)imin((int)((pg + (th >> 2)) >> 2), 63);
    unsigned b5 = (unsigned)imin((int)((pb + (th >> 1)) >> 3), 31);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* m opaque pixels of one colour from p (ph: dither phase of p[0], the screen column & 3; bay: dither row): four
   values repeat, written two at a time. */
static void put_flat(uint16_t *p, int m, unsigned ph, const uint8_t *bay, unsigned pr, unsigned pg, unsigned pb)
{
    uint16_t q[4];
    for (int k = 0; k < 4; k++) {
        q[k] = pm_opaque(pr, pg, pb, bay[(ph + (unsigned)k) & 3]);
    }
    int k = 0;
    if (m > 0 && ((uintptr_t)p & 2)) {
        p[0] = q[0];
        k = 1;
    }
    union { uint16_t h[2]; uint32_t w; } w0 = { { q[k & 3], q[(k + 1) & 3] } }, w1 = { { q[(k + 2) & 3], q[(k + 3) & 3] } };
    u32a *w = (u32a *)(p + k);
    for (; k + 3 < m; k += 4, w += 2) {
        w[0] = w0.w;
        w[1] = w1.w;
    }
    for (; k < m; k++) {
        p[k] = q[k & 3];
    }
}

/* m opaque pixels from p whose levels (8.16) start at v1, v2, v3 and step by i1, i2, i3 (ph, bay: as put_flat),
   as put_pm writes them. The pixels of one dither phase are done together (every fourth one), so the threshold is
   a constant of the inner loop. inside: every level of the run is known to lie in 0..255, the clamps are left out
   and the threshold is added before the shift ((v + (h << 16)) >> 16 == (v >> 16) + h). */
static void run_opaque(uint16_t *p, int m, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t i1, uint32_t i2,
                       uint32_t i3, unsigned ph, const uint8_t *bay, bool inside)
{
    uint32_t s1 = i1 * 4u, s2 = i2 * 4u, s3 = i3 * 4u;
    for (int q = 0; q < 4 && q < m; q++) {
        unsigned th = bay[(ph + (unsigned)q) & 3], h1 = th >> 1, h2 = th >> 2;
        uint32_t a1 = v1 + (uint32_t)q * i1, a2 = v2 + (uint32_t)q * i2, a3 = v3 + (uint32_t)q * i3;
        uint16_t *pp = p + q;
        int cnt = (m - q + 3) >> 2;
        if (inside) {
            uint32_t d1 = (uint32_t)h1 << 16, d2 = (uint32_t)h2 << 16;
            for (int k = 0; k < cnt; k++, pp += 4) {
                unsigned r5 = (unsigned)imin((int)((a1 + d1) >> 19), 31);
                unsigned g6 = (unsigned)imin((int)((a2 + d2) >> 18), 63);
                unsigned b5 = (unsigned)imin((int)((a3 + d1) >> 19), 31);
                *pp = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                a1 += s1;
                a2 += s2;
                a3 += s3;
            }
        } else {
            for (int k = 0; k < cnt; k++, pp += 4) {
                unsigned pr = (unsigned)imax(0, imin((int32_t)a1 >> 16, 255));
                unsigned pg = (unsigned)imax(0, imin((int32_t)a2 >> 16, 255));
                unsigned pb = (unsigned)imax(0, imin((int32_t)a3 >> 16, 255));
                unsigned r5 = (unsigned)imin((int)((pr + h1) >> 3), 31);
                unsigned g6 = (unsigned)imin((int)((pg + h2) >> 2), 63);
                unsigned b5 = (unsigned)imin((int)((pb + h1) >> 3), 31);
                *pp = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                a1 += s1;
                a2 += s2;
                a3 += s3;
            }
        }
    }
}

/* The stops in fixed point, converted once and reused while the same stops come back (a gradient is drawn row by
   row, or piece by piece, with the same stops): the conversion depends on the stops' values only. sh[k]: span k
   (between stops k and k + 1) is 2^sh[k] wide in 40.24, so its divides are shifts; -1 otherwise. One compositor
   at a time draws (slate_gfx.h), so one memo serves. */
typedef struct {
    int ns;                                    /* 0: empty */
    uint32_t gen;                              /* changes whenever the stops change */
    uint32_t key[2 * SLATE_GRADIENT_STOPS_MAX];   /* the stops' bits */
    fx_stop_t st[SLATE_GRADIENT_STOPS_MAX];
    int8_t sh[SLATE_GRADIENT_STOPS_MAX];
    bool small;                                /* every at within +-2^29 (see grad_fast) */
    bool fast[SLATE_GRADIENT_STOPS_MAX];       /* span k: both stops opaque and sh[k] >= 16 (see grad_fast) */
    int32_t at32[SLATE_GRADIENT_STOPS_MAX];    /* at, when small */
} fx_memo_t;
static fx_memo_t fx_memo;

static inline const fx_memo_t *fx_stops(const slate_stop_t *stops, int ns)
{
    bool same = fx_memo.ns == ns;
    for (int i = 0; i < ns && same; i++) {   /* the same bits convert to the same values */
        union { float f; uint32_t u; } at = { stops[i].at };
        same = fx_memo.key[2 * i] == stops[i].argb && fx_memo.key[2 * i + 1] == at.u;
    }
    if (same) {
        return &fx_memo;
    }
    fx_memo.small = true;
    for (int i = 0; i < ns; i++) {
        unsigned A = stops[i].argb >> 24;
        union { float f; uint32_t u; } at = { stops[i].at };
        fx_memo.key[2 * i] = stops[i].argb;
        fx_memo.key[2 * i + 1] = at.u;
        fx_memo.st[i].at = to_fx(stops[i].at, FX_T_MAX);
        fx_memo.st[i].c[0] = (uint8_t)A;
        fx_memo.st[i].c[1] = (uint8_t)div255(((stops[i].argb >> 16) & 255) * A);
        fx_memo.st[i].c[2] = (uint8_t)div255(((stops[i].argb >> 8) & 255) * A);
        fx_memo.st[i].c[3] = (uint8_t)div255((stops[i].argb & 255) * A);
        fx_memo.small = fx_memo.small && fx_memo.st[i].at >= -(1 << 29) && fx_memo.st[i].at <= (1 << 29);
        fx_memo.at32[i] = (int32_t)fx_memo.st[i].at;
    }
    for (int i = 0; i + 1 < ns; i++) {
        int64_t wd = fx_memo.st[i + 1].at - fx_memo.st[i].at;
        int sh = -1;
        if (wd > 0 && (wd & (wd - 1)) == 0) {
            for (sh = 0; ((int64_t)1 << sh) != wd; sh++) {
            }
        }
        fx_memo.sh[i] = (int8_t)sh;
        fx_memo.fast[i] = sh >= 16 && fx_memo.st[i].c[0] == 255 && fx_memo.st[i + 1].c[0] == 255;
    }
    fx_memo.ns = ns;
    fx_memo.gen++;
    return &fx_memo;
}

/* n / wd as C divides (toward zero), wd > 0; a shift when wd is 2^sh (sh >= 0). */
static inline int64_t div_wd(int64_t n, int64_t wd, int sh)
{
    if (sh >= 0) {
        return n >= 0 ? n >> sh : -((-n) >> sh);
    }
    return n / wd;
}

/* v / 2^k as C divides (toward zero), k >= 0. */
static inline int32_t sdiv2k(int32_t v, int k)
{
    return v >= 0 ? v >> k : -((-v) >> k);
}

/* The common case in 32-bit arithmetic: no coverage, the m pixels from p (screen column X of p[0]) all in one span
   of two opaque stops 2^sh wide (sh >= 16), and t, its step and the columns small enough that every value below
   fits in 32 bits. The values are then exactly those of the general path of sw_gradient_row (the same integer
   expressions, none of which overflows; T and D are to_fx's, which neither clamps nor needs 64 bits here), only
   cheaper to get. false: not that case, nothing written. */
static bool grad_fast(const fx_memo_t *mm, int ns, float t0, float dt, int X, int m, uint16_t *p, const uint8_t *bay)
{
    float st = t0 * FX_ONE, sd = dt * FX_ONE;
    if (!mm->small || !(st > -536870912.0f && st < 536870912.0f && sd > -524288.0f && sd < 524288.0f) || X < -1024
        || X > 1023 || m > 1024) {
        return false;   /* (also NaN) */
    }
    int32_t T = (int32_t)(st >= 0 ? st + 0.5f : st - 0.5f), D = (int32_t)(sd >= 0 ? sd + 0.5f : sd - 0.5f);
    int32_t t = T + X * D, tl = t + (m - 1) * D;   /* |T| <= 2^29, |D| <= 2^19, |X|, m <= 2^10: |t|, |tl| < 2^31 */
    const int32_t *at = mm->at32;
    if (t <= at[0] || t >= at[ns - 1]) {
        return false;
    }
    int seg = 0;
    while (seg < ns - 2 && t >= at[seg + 1]) {
        seg++;
    }
    if (D > 0 ? tl >= at[seg + 1] : D < 0 && tl < at[seg]) {
        return false;   /* the run leaves the span */
    }
    int sh = mm->sh[seg];
    if (!mm->fast[seg]) {
        return false;
    }
    int32_t fs;   /* D * 2^24 / 2^sh */
    if (sh >= 24) {
        fs = sdiv2k(D, sh - 24);
    } else {
        if (D < -(1 << (sh - 2)) || D > (1 << (sh - 2))) {
            return false;   /* |fs| would pass 2^22 */
        }
        fs = D * (1 << (24 - sh));
    }
    int32_t f0 = sdiv2k(T - at[seg], sh - 16);   /* (T - at) * 2^16 / 2^sh, |T - at| <= 2^30 */
    const uint8_t *c0 = mm->st[seg].c, *c1 = mm->st[seg + 1].c;
    int32_t d1 = (int32_t)c1[1] - c0[1], d2 = (int32_t)c1[2] - c0[2], d3 = (int32_t)c1[3] - c0[3];
    int32_t I1 = (d1 * fs) >> 8, I2 = (d2 * fs) >> 8, I3 = (d3 * fs) >> 8;   /* |d * fs| < 2^30: no clamp */
    uint32_t xu = (uint32_t)X, fu = (uint32_t)f0;
    uint32_t v1 = (uint32_t)c0[1] * 65536u + (uint32_t)d1 * fu + 32768u + xu * (uint32_t)I1;
    uint32_t v2 = (uint32_t)c0[2] * 65536u + (uint32_t)d2 * fu + 32768u + xu * (uint32_t)I2;
    uint32_t v3 = (uint32_t)c0[3] * 65536u + (uint32_t)d3 * fu + 32768u + xu * (uint32_t)I3;
    unsigned ph = (unsigned)X & 3;
    if ((I1 | I2 | I3) == 0) {
        put_flat(p, m, ph, bay, (unsigned)imax(0, imin((int32_t)v1 >> 16, 255)),
                 (unsigned)imax(0, imin((int32_t)v2 >> 16, 255)), (unsigned)imax(0, imin((int32_t)v3 >> 16, 255)));
        return true;
    }
    /* every level of the run in 0..255: first and last within 0..0xFFFFFF (|I| <= 2^20, so no overflow) */
    uint32_t k = (uint32_t)(m - 1);
    bool inside = ((uint32_t)I1 + (1u << 20) <= (2u << 20)) & ((uint32_t)I2 + (1u << 20) <= (2u << 20))
                  & ((uint32_t)I3 + (1u << 20) <= (2u << 20));
    inside = inside && (v1 <= 0xFFFFFFu) & (v2 <= 0xFFFFFFu) & (v3 <= 0xFFFFFFu)
             & (v1 + k * (uint32_t)I1 <= 0xFFFFFFu) & (v2 + k * (uint32_t)I2 <= 0xFFFFFFu)
             & (v3 + k * (uint32_t)I3 <= 0xFFFFFFu);
    run_opaque(p, m, v1, v2, v3, (uint32_t)I1, (uint32_t)I2, (uint32_t)I3, ph, bay, inside);
    return true;
}

static void sw_gradient_row(slate_buf_t *b, int x, int y, int n, float t0, float dt, const slate_stop_t *stops, int ns,
                            const uint8_t *cov)
{
    if (y < 0 || y >= b->h || ns <= 0 || n <= 0) {
        return;
    }
    if (ns > SLATE_GRADIENT_STOPS_MAX) {
        ns = SLATE_GRADIENT_STOPS_MAX;
    }
    const fx_memo_t *mm = fx_stops(stops, ns);
    const fx_stop_t *st = mm->st;
    if (!cov && ns >= 2) {
        int i0 = imax(0, -x), m = imin(n, b->w - x) - i0;
        int64_t Xs = (int64_t)x + i0 + b->ox;
        if (m <= 0 || grad_fast(mm, ns, t0, dt, Xs < -1024 || Xs > 1023 ? 2048 : (int)Xs, m,
                                b->px + (size_t)y * b->stride + x + i0, bayer4[(y + b->oy) & 3])) {
            return;
        }
    }
    /* Every value below is a function of the screen column X = x + ox + i and of (t0, dt) only, never of where
       the row starts: t(X) = T + X * D (40.24), and inside a stop span each channel is the affine form C + X * I (8.16,
       modular uint32: the true value fits, so the wrap-around is exact), so a row drawn in pieces, clipped or
       split across buffers gives the same pixels. */
    int64_t T = to_fx(t0, FX_T_MAX), D = to_fx(dt, FX_D_MAX);
    uint16_t *row = b->px + (size_t)y * b->stride;
    const uint8_t *bay = bayer4[(y + b->oy) & 3];
    int i = imax(0, -x), iend = imin(n, b->w - x);   /* the visible pixels; the loops below have no bounds tests */
    int64_t X0 = (int64_t)x + b->ox;                  /* screen column of pixel i = 0 */
    int fseg = -1;                                    /* span whose fs, f0 are cached (-1: none) */
    int64_t fs = 0, f0 = 0;

    while (i < iend) {
        int64_t X = X0 + i;
        int64_t t = T + X * D;
        int64_t cnt;                       /* pixels from i that stay in the same region */
        int64_t left = iend - i;
        /* t of the last pixel: when it is still in the region (t is monotonic), so is every pixel, cnt = left, and
           the divide is not needed (it would give at least left) */
        bool all = left <= 65536;
        int64_t tl = all ? t + (left - 1) * D : 0;
        int seg;                           /* -1: constant before, ns-1: constant after, else span seg..seg+1 */
        if (ns == 1 || t <= st[0].at) {
            seg = -1;
            cnt = D > 0 && ns > 1 ? (all && tl <= st[0].at ? left : (st[0].at - t) / D + 1) : left;
        } else if (t >= st[ns - 1].at) {
            seg = ns - 1;
            cnt = D < 0 ? (all && tl >= st[ns - 1].at ? left : (t - st[ns - 1].at) / -D + 1) : left;
        } else {
            seg = 0;
            while (seg < ns - 2 && t >= st[seg + 1].at) {
                seg++;
            }
            if (D > 0) {
                cnt = all && tl < st[seg + 1].at ? left : (st[seg + 1].at - t + D - 1) / D;
            } else if (D < 0) {
                cnt = all && tl >= st[seg].at ? left : (t - st[seg].at) / -D + 1;
            } else {
                cnt = left;
            }
        }
        if (cnt > left) {
            cnt = left;
        }
        if (cnt < 1) {
            cnt = 1;
        }
        int m = (int)cnt;
        unsigned ph = (unsigned)X & 3;     /* dither phase of pixel i */
        int64_t wd = seg >= 0 && seg < ns - 1 ? (int64_t)st[seg + 1].at - st[seg].at : 0;
        if (seg < 0 || seg == ns - 1 || wd <= 0) {
            const uint8_t *c = seg < 0 ? st[0].c : st[seg == ns - 1 ? ns - 1 : seg + 1].c;
            if (!cov && c[0] == 255) {
                put_flat(&row[x + i], m, ph, bay, c[1], c[2], c[3]);
                i += m;
                continue;
            }
            for (int k = 0; k < m; k++, i++) {
                put_pm(&row[x + i], c[0], c[1], c[2], c[3], cov ? cov[i] : 255, bay[(x + i + b->ox) & 3]);
            }
            continue;
        }
        /* inside a span: per channel v(X) = C + X * I, both from the anchor X = 0 only. Two divides per span
           (cached across the regions of this call; shifts for a span 2^k wide), shared by the four channels:
           Fs = D / wd in 8.24 per pixel, F0 = (T - at) / wd in 16.16 at X = 0. I keeps 8 extra bits until the shift,
           so its error is about 2^-16 level per pixel and stays far below a level across a whole screen of
           columns. */
        if (seg != fseg) {
            fseg = seg;
            fs = D != 0 ? div_wd(D * 16777216, wd, mm->sh[seg]) : 0;
            fs = fs > ((int64_t)1 << 46) ? ((int64_t)1 << 46) : fs < -((int64_t)1 << 46) ? -((int64_t)1 << 46) : fs;
            f0 = div_wd((T - st[seg].at) * 65536, wd, mm->sh[seg]);
        }
        const uint8_t *c0 = st[seg].c, *c1 = st[seg + 1].c;
        uint32_t v[4], inc[4];
        for (int k = 0; k < 4; k++) {
            int32_t d = (int32_t)c1[k] - c0[k];
            int64_t I = ((int64_t)d * fs) >> 8;   /* level 8.16 per pixel (|fs| clamped where I would clamp anyway) */
            I = I > (1 << 30) ? (1 << 30) : I < -(1 << 30) ? -(1 << 30) : I;
            inc[k] = (uint32_t)I;
            /* C = c0 * 65536 + d * f0 + 32768 taken modulo 2^32, as v is */
            v[k] = (uint32_t)c0[k] * 65536u + (uint32_t)d * (uint32_t)f0 + 32768u + (uint32_t)X * inc[k];
        }
        if (!cov && c0[0] == 255 && c1[0] == 255) {
            /* opaque: A is 255 all along (its I is 0), so each pixel is pm_opaque of its clamped levels */
            uint16_t *p = &row[x + i];
            if (inc[1] == 0 && inc[2] == 0 && inc[3] == 0) {
                put_flat(p, m, ph, bay, (unsigned)imax(0, imin((int32_t)v[1] >> 16, 255)),
                         (unsigned)imax(0, imin((int32_t)v[2] >> 16, 255)),
                         (unsigned)imax(0, imin((int32_t)v[3] >> 16, 255)));
            } else {
                /* the levels are affine: when the first and last of the run lie in 0..255 (exact arithmetic), so
                   do the ones between, and the clamps do nothing */
                bool inside = true;
                for (int k = 1; k < 4; k++) {
                    int64_t a0 = (int32_t)v[k], a1 = a0 + (int64_t)(m - 1) * (int32_t)inc[k];
                    inside = inside && a0 >= 0 && a0 <= 0xFFFFFF && a1 >= 0 && a1 <= 0xFFFFFF;
                }
                run_opaque(p, m, v[1], v[2], v[3], inc[1], inc[2], inc[3], ph, bay, inside);
            }
            i += m;
            continue;
        }
        for (int k = 0; k < m; k++, i++) {
            unsigned A = (unsigned)imax(0, imin((int32_t)v[0] >> 16, 255));
            unsigned pr = (unsigned)imax(0, imin((int32_t)v[1] >> 16, 255));
            unsigned pg = (unsigned)imax(0, imin((int32_t)v[2] >> 16, 255));
            unsigned pb = (unsigned)imax(0, imin((int32_t)v[3] >> 16, 255));
            put_pm(&row[x + i], A, imin((int)pr, (int)A), imin((int)pg, (int)A), imin((int)pb, (int)A),
                   cov ? cov[i] : 255, bay[(x + i + b->ox) & 3]);
            v[0] += inc[0]; v[1] += inc[1]; v[2] += inc[2]; v[3] += inc[3];
        }
    }
}

/* ---- the pieces of one row (slate_sw.h) ---- */

bool slate_sw_gradient(const slate_gfx_ops_t *g)
{
    return g && g->gradient_row == sw_gradient_row;
}

bool slate_sw_grow_begin(slate_sw_grow_t *r, slate_buf_t *b, int y, const slate_stop_t *stops, int ns)
{
    if (y < 0 || y >= b->h || ns <= 0 || !stops) {
        return false;
    }
    r->b = b;
    r->y = y;
    r->stops = stops;
    r->ns = ns > SLATE_GRADIENT_STOPS_MAX ? SLATE_GRADIENT_STOPS_MAX : ns;
    r->gen = fx_stops(stops, r->ns)->gen;
    return true;
}

void slate_sw_grow_piece(const slate_sw_grow_t *r, int x, int n, float t0, float dt, const uint8_t *cov)
{
    slate_buf_t *b = r->b;
    if (!cov && r->ns >= 2 && n > 0 && fx_memo.gen == r->gen && fx_memo.ns == r->ns) {
        int i0 = imax(0, -x), m = imin(n, b->w - x) - i0;
        int64_t Xs = (int64_t)x + i0 + b->ox;
        if (m <= 0 || grad_fast(&fx_memo, r->ns, t0, dt, Xs < -1024 || Xs > 1023 ? 2048 : (int)Xs, m,
                                b->px + (size_t)r->y * b->stride + x + i0, bayer4[(r->y + b->oy) & 3])) {
            return;
        }
    }
    sw_gradient_row(b, x, r->y, n, t0, dt, r->stops, r->ns, cov);
}

#endif /* SLATE_HAS_GRADIENTS */

/* ---- blur (also the alpha blur of shadows) ---- */

#if SLATE_GFX_BLUR

#define VBLOCK 32   /* columns handled together by a vertical pass */

/* sum / win rounded, as sum * inv >> 24 with inv = 2^24 / win + 1. For a sum of at most win values of at most max
   it is at most max (sum * inv <= max * 2^24 + max * win, and max * win + 2^23 < 2^24 for win <= 511), so the
   results below need no clamp; the product stays below 2^32 for the same reason. */
static inline uint32_t recip_round(uint32_t sum, uint32_t inv)
{
    return (sum * inv + (1u << 23)) >> 24;
}

static inline uint16_t px565(unsigned sr, unsigned sg, unsigned sb, uint32_t inv)
{
    return (uint16_t)((recip_round(sr, inv) << 11) | (recip_round(sg, inv) << 5) | recip_round(sb, inv));
}

/* n bytes at a equal those at b? Word by word when the two have the same alignment (rows of a width that is a
   multiple of 4, blocks of VBLOCK), in runs of eight words without a branch inside (a branch per word costs
   more than the compare). */
static inline bool same_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i = 0;
    uint32_t d = 0;
    if ((((uintptr_t)a ^ (uintptr_t)b) & 3) == 0) {
        for (; i < n && ((uintptr_t)(a + i) & 3); i++) {
            d |= (uint32_t)(a[i] ^ b[i]);
        }
        for (; i + 32 <= n; i += 32) {
            const u32a *x = (const u32a *)(a + i), *y = (const u32a *)(b + i);
            d |= (x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3]) | (x[4] ^ y[4]) | (x[5] ^ y[5])
                 | (x[6] ^ y[6]) | (x[7] ^ y[7]);
            if (d) {
                return false;
            }
        }
        for (; i + 4 <= n; i += 4) {
            d |= *(const u32a *)(a + i) ^ *(const u32a *)(b + i);
        }
    }
    for (; i < n; i++) {
        d |= (uint32_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

/* Copies n bytes (no overlap), word by word when the two have the same alignment. */
static inline void copy_bytes(uint8_t *d, const uint8_t *s, size_t n)
{
    size_t i = 0;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
        for (; i < n && ((uintptr_t)(d + i) & 3); i++) {
            d[i] = s[i];
        }
        for (; i + 4 <= n; i += 4) {
            *(u32a *)(d + i) = *(const u32a *)(s + i);
        }
    }
    for (; i < n; i++) {
        d[i] = s[i];
    }
}

/* Is row (or block) a equal to b in both planes (alpha only when !color)? */
static inline bool same_px(const uint16_t *pa, const uint16_t *pb, const uint8_t *aa, const uint8_t *ab, int n,
                           int color)
{
    return same_bytes(aa, ab, (size_t)n) && (!color || same_bytes((const uint8_t *)pa, (const uint8_t *)pb, (size_t)n * 2));
}

/* Horizontal box pass of width 2r+1 over each of the h rows of w pixels, from (si, sa) to (di, da). si == NULL:
   alpha only. Each row is a function of its source row only, so a row equal to the one above takes that row's
   result (in a shadow or a card most rows repeat). The sliding sum is split where the window enters and leaves
   the row, so the loops have no bounds tests. */
static inline __attribute__((always_inline)) void hpass(const uint16_t *si, const uint8_t *sa, uint16_t *di,
                                                        uint8_t *da, int w, int h, int r, int color)
{
    uint32_t inv = (uint32_t)((1u << 24) / (unsigned)(2 * r + 1)) + 1;   /* x * inv >> 24 ~ x / win */
    for (int l = 0; l < h; l++) {
        size_t base = (size_t)l * w;
        if (l > 0 && same_px(color ? si + base : NULL, color ? si + base - w : NULL, sa + base, sa + base - w, w, color)) {
            copy_bytes(da + base, da + base - w, (size_t)w);
            if (color) {
                copy_bytes((uint8_t *)(di + base), (const uint8_t *)(di + base - w), (size_t)w * 2);
            }
            continue;
        }
        const uint8_t *A = sa + base;
        uint8_t *dA = da + base;
        const uint16_t *S = color ? si + base : NULL;
        uint16_t *dS = color ? di + base : NULL;
        unsigned sr = 0, sg = 0, sb = 0, sA = 0;
        for (int i = 0; i <= r && i < w; i++) {
            if (color) {
                uint16_t q = S[i];
                sr += (q >> 11) & 31; sg += (q >> 5) & 63; sb += q & 31;
            }
            sA += A[i];
        }
        int i = 0;
        if (r < w - r - 1) {
            for (; i < r; i++) {             /* in = i + r + 1 < w, out = i - r < 0 */
                if (color) {
                    dS[i] = px565(sr, sg, sb, inv);
                    uint16_t q = S[i + r + 1];
                    sr += (q >> 11) & 31; sg += (q >> 5) & 63; sb += q & 31;
                }
                dA[i] = (uint8_t)recip_round(sA, inv);
                sA += A[i + r + 1];
            }
            for (; i < w - r - 1; i++) {     /* both inside */
                if (color) {
                    dS[i] = px565(sr, sg, sb, inv);
                    uint16_t q = S[i + r + 1], o = S[i - r];
                    sr += ((q >> 11) & 31) - ((o >> 11) & 31);
                    sg += ((q >> 5) & 63) - ((o >> 5) & 63);
                    sb += (q & 31) - (o & 31);
                }
                dA[i] = (uint8_t)recip_round(sA, inv);
                sA += A[i + r + 1] - A[i - r];
            }
        } else {
            for (; i < w - r - 1; i++) {     /* in < w, out < 0 (r >= w - r - 1) */
                if (color) {
                    dS[i] = px565(sr, sg, sb, inv);
                    uint16_t q = S[i + r + 1];
                    sr += (q >> 11) & 31; sg += (q >> 5) & 63; sb += q & 31;
                }
                dA[i] = (uint8_t)recip_round(sA, inv);
                sA += A[i + r + 1];
            }
            for (; i < r && i < w; i++) {    /* neither */
                if (color) {
                    dS[i] = px565(sr, sg, sb, inv);
                }
                dA[i] = (uint8_t)recip_round(sA, inv);
            }
        }
        for (; i < w; i++) {                 /* in >= w, out >= 0 */
            if (color) {
                dS[i] = px565(sr, sg, sb, inv);
                uint16_t o = S[i - r];
                sr -= (o >> 11) & 31; sg -= (o >> 5) & 63; sb -= o & 31;
            }
            dA[i] = (uint8_t)recip_round(sA, inv);
            sA -= A[i - r];
        }
    }
}

/* Vertical pass: columns in blocks of VBLOCK, each block swept top to bottom with one running sum per column,
   so memory is read and written row by row (a block row is VBLOCK contiguous pixels). When the row entering the
   window equals the one leaving it, the sums do not change and the next output row is the one just written
   (most rows of a shadow or a card). */
static inline __attribute__((always_inline)) void vpass(const uint16_t *si, const uint8_t *sa, uint16_t *di,
                                                        uint8_t *da, int w, int h, int r, int color)
{
    uint32_t inv = (uint32_t)((1u << 24) / (unsigned)(2 * r + 1)) + 1;
    for (int c0 = 0; c0 < w; c0 += VBLOCK) {
        int nc = imin(VBLOCK, w - c0);
        unsigned sr[VBLOCK] = { 0 }, sg[VBLOCK] = { 0 }, sb[VBLOCK] = { 0 }, sA[VBLOCK] = { 0 };
        for (int j = 0; j <= r && j < h; j++) {
            size_t o = (size_t)j * w + c0;
            for (int k = 0; k < nc; k++) {
                if (color) {
                    uint16_t c = si[o + k];
                    sr[k] += (c >> 11) & 31; sg[k] += (c >> 5) & 63; sb[k] += c & 31;
                }
                sA[k] += sa[o + k];
            }
        }
        bool repeat = false;   /* the sums are those of the row above: copy its output */
        for (int j = 0; j < h; j++) {
            size_t o = (size_t)j * w + c0;
            if (repeat) {
                copy_bytes(da + o, da + o - w, (size_t)nc);
                if (color) {
                    copy_bytes((uint8_t *)(di + o), (const uint8_t *)(di + o - w), (size_t)nc * 2);
                }
            } else {
                for (int k = 0; k < nc; k++) {
                    if (color) {
                        di[o + k] = px565(sr[k], sg[k], sb[k], inv);
                    }
                    da[o + k] = (uint8_t)recip_round(sA[k], inv);
                }
            }
            int in = j + r + 1, out = j - r;
            if (in < h && out >= 0) {
                size_t oi = (size_t)in * w + c0, oo = (size_t)out * w + c0;
                repeat = same_px(color ? si + oi : NULL, color ? si + oo : NULL, sa + oi, sa + oo, nc, color);
                if (!repeat) {
                    for (int k = 0; k < nc; k++) {
                        if (color) {
                            uint16_t c = si[oi + k], d = si[oo + k];
                            sr[k] += ((c >> 11) & 31) - ((d >> 11) & 31);
                            sg[k] += ((c >> 5) & 63) - ((d >> 5) & 63);
                            sb[k] += (c & 31) - (d & 31);
                        }
                        sA[k] += sa[oi + k] - sa[oo + k];
                    }
                }
                continue;
            }
            repeat = false;
            if (in < h) {
                size_t oi = (size_t)in * w + c0;
                for (int k = 0; k < nc; k++) {
                    if (color) {
                        uint16_t c = si[oi + k];
                        sr[k] += (c >> 11) & 31; sg[k] += (c >> 5) & 63; sb[k] += c & 31;
                    }
                    sA[k] += sa[oi + k];
                }
            }
            if (out >= 0) {
                size_t oo = (size_t)out * w + c0;
                for (int k = 0; k < nc; k++) {
                    if (color) {
                        uint16_t c = si[oo + k];
                        sr[k] -= (c >> 11) & 31; sg[k] -= (c >> 5) & 63; sb[k] -= c & 31;
                    }
                    sA[k] -= sa[oo + k];
                }
            }
        }
    }
}

/* One box pass of width 2r+1 over a line of n values (hpass for one row, alpha only): the sliding sum is split
   where the window enters and leaves the line, so the loops have no bounds tests. */
static void box_line(const uint8_t *A, uint8_t *D, int n, int r, uint32_t inv)
{
    int e1 = imin(r, n - r - 1), e2 = imax(r, n - r - 1);
    uint32_t s = 0;
    for (int i = 0; i <= r && i < n; i++) {
        s += A[i];
    }
    int i = 0;
    for (; i < e1; i++) {
        D[i] = (uint8_t)recip_round(s, inv);
        s += A[i + r + 1];
    }
    if (r < n - r - 1) {
        for (; i < e2; i++) {
            D[i] = (uint8_t)recip_round(s, inv);
            s += (uint32_t)A[i + r + 1] - A[i - r];
        }
    } else {
        for (; i < e2 && i < n; i++) {
            D[i] = (uint8_t)recip_round(s, inv);
        }
    }
    for (; i < n; i++) {
        D[i] = (uint8_t)recip_round(s, inv);
        s -= A[i - r];
    }
}

/* The alpha-only passes (shadows), as hpass and vpass with color = 0 but written on their own so that the loops
   stay small: the same sums, the same results. A row equal to the one above takes its result. */
static void hpass_a(const uint8_t *src, uint8_t *dst, int w, int h, int r)
{
    uint32_t inv = (uint32_t)((1u << 24) / (unsigned)(2 * r + 1)) + 1;
    for (int l = 0; l < h; l++) {
        const uint8_t *A = src + (size_t)l * w;
        uint8_t *D = dst + (size_t)l * w;
        if (l > 0 && same_bytes(A, A - w, (size_t)w)) {
            copy_bytes(D, D - w, (size_t)w);
        } else {
            box_line(A, D, w, r, inv);
        }
    }
}

static void vpass_a(const uint8_t *src, uint8_t *dst, int w, int h, int r)
{
    uint32_t inv = (uint32_t)((1u << 24) / (unsigned)(2 * r + 1)) + 1;
    for (int c0 = 0; c0 < w; c0 += VBLOCK) {
        int nc = imin(VBLOCK, w - c0);
        uint32_t sum[VBLOCK] = { 0 };
        for (int j = 0; j <= r && j < h; j++) {
            const uint8_t *S = src + (size_t)j * w + c0;
            for (int k = 0; k < nc; k++) {
                sum[k] += S[k];
            }
        }
        bool repeat = false;
        for (int j = 0; j < h; j++) {
            uint8_t *D = dst + (size_t)j * w + c0;
            if (repeat) {
                copy_bytes(D, D - w, (size_t)nc);
            } else {
                for (int k = 0; k < nc; k++) {
                    D[k] = (uint8_t)recip_round(sum[k], inv);
                }
            }
            int in = j + r + 1, out = j - r;
            const uint8_t *I = in < h ? src + (size_t)in * w + c0 : NULL, *O = out >= 0 ? src + (size_t)out * w + c0 : NULL;
            if (I && O) {
                repeat = same_bytes(I, O, (size_t)nc);
                if (!repeat) {
                    for (int k = 0; k < nc; k++) {
                        sum[k] += (uint32_t)I[k] - O[k];
                    }
                }
            } else {
                repeat = false;
                if (I) {
                    for (int k = 0; k < nc; k++) {
                        sum[k] += I[k];
                    }
                }
                if (O) {
                    for (int k = 0; k < nc; k++) {
                        sum[k] -= O[k];
                    }
                }
            }
        }
    }
}

#define BLUR_LINE 384   /* longest line the fused passes take (buffers of it on the stack) */

/* The alpha-only blur with the three passes of each direction done one line at a time in buffers on the stack,
   so the image (often in external RAM) is read and written once per direction instead of three times. Each pass
   works on one line at a time either way, so the result is that of the three whole-image passes.
   Rows: a row equal to the one above has the same result, so only the distinct results are kept, one after the
   other in tmp, with idx[y] naming the one of row y (a shadow's rows repeat a lot). Columns: column x is gathered
   from those results (few, so they stay in the cache), and a column equal to the one before takes its result;
   each result is written back to alpha. w, h <= BLUR_LINE. */
static void blur_a_fused(uint8_t *alpha, uint8_t *ta, int w, int h, int r)
{
    uint32_t inv = (uint32_t)((1u << 24) / (unsigned)(2 * r + 1)) + 1;
    uint16_t idx[BLUR_LINE];
    uint8_t ga[BLUR_LINE], gb[BLUR_LINE], t[BLUR_LINE], res[BLUR_LINE];
    int nd = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *A = alpha + (size_t)y * w;
        if (y > 0 && same_bytes(A, A - w, (size_t)w)) {
            idx[y] = idx[y - 1];
            continue;
        }
        box_line(A, t, w, r, inv);
        box_line(t, ga, w, r, inv);
        box_line(ga, ta + (size_t)nd * w, w, r, inv);
        idx[y] = (uint16_t)nd++;
    }
    uint8_t *cur = ga, *prev = gb;
    for (int x = 0; x < w; x++) {
        for (int j = 0; j < h; j++) {
            cur[j] = ta[(size_t)idx[j] * w + x];
        }
        if (x == 0 || !same_bytes(cur, prev, (size_t)h)) {
            box_line(cur, t, h, r, inv);
            box_line(t, prev, h, r, inv);   /* the column before is not needed any more */
            box_line(prev, res, h, r, inv);
        }
        uint8_t *q = cur;
        cur = prev;
        prev = q;
        uint8_t *D = alpha + x;
        for (int j = 0; j < h; j++) {
            D[(size_t)j * w] = res[j];
        }
    }
}

static void sw_blur(uint16_t *px, uint8_t *alpha, int w, int h, int r, void *tmp)
{
    if (r <= 0 || w <= 0 || h <= 0 || !alpha) {
        return;
    }
    if (r > SLATE_BLUR_RADIUS_MAX) {
        r = SLATE_BLUR_RADIUS_MAX;
    }
    size_t cnt = (size_t)w * h;
    if (!px) {   /* alpha only: tmp is w*h bytes */
        uint8_t *ta = (uint8_t *)tmp;
        if (w <= BLUR_LINE && h <= BLUR_LINE) {
            blur_a_fused(alpha, ta, w, h, r);
            return;
        }
        hpass_a(alpha, ta, w, h, r);
        hpass_a(ta, alpha, w, h, r);
        hpass_a(alpha, ta, w, h, r);
        vpass_a(ta, alpha, w, h, r);
        vpass_a(alpha, ta, w, h, r);
        vpass_a(ta, alpha, w, h, r);
        return;
    }
    uint16_t *tp = (uint16_t *)tmp;
    uint8_t *ta = (uint8_t *)tmp + cnt * 2;
    /* three horizontal passes then three vertical ones; an even count of ping-pongs ends in the image */
    hpass(px, alpha, tp, ta, w, h, r, 1);
    hpass(tp, ta, px, alpha, w, h, r, 1);
    hpass(px, alpha, tp, ta, w, h, r, 1);
    vpass(tp, ta, px, alpha, w, h, r, 1);
    vpass(px, alpha, tp, ta, w, h, r, 1);
    vpass(tp, ta, px, alpha, w, h, r, 1);
}

#endif /* SLATE_GFX_BLUR */

/* ---- table ---- */

#if !SLATE_HAS_GRADIENTS
#define sw_gradient_row NULL
#endif
#if !SLATE_GFX_BLUR
#define sw_blur NULL
#endif

static const slate_gfx_ops_t sw_ops = {
    sw_fill, sw_fill_a, sw_fill_mask, sw_copy, sw_copy_a, sw_gradient_row, sw_blur,
};
static slate_gfx_ops_t active = {
    sw_fill, sw_fill_a, sw_fill_mask, sw_copy, sw_copy_a, sw_gradient_row, sw_blur,
};

const slate_gfx_ops_t *slate_gfx(void)
{
    return &active;
}

void slate_gfx_override(const slate_gfx_ops_t *hw)
{
    active = sw_ops;
    if (!hw) {
        return;
    }
    if (hw->fill) active.fill = hw->fill;
    if (hw->fill_a) active.fill_a = hw->fill_a;
    if (hw->fill_mask) active.fill_mask = hw->fill_mask;
    if (hw->copy) active.copy = hw->copy;
    if (hw->copy_a) active.copy_a = hw->copy_a;
#if SLATE_HAS_GRADIENTS
    if (hw->gradient_row) active.gradient_row = hw->gradient_row;
#endif
#if SLATE_GFX_BLUR
    if (hw->blur) active.blur = hw->blur;
#endif
}
