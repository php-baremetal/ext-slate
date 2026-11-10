/* Anti-aliased shapes and text of Slate: see raster.h.

   Every shape is drawn row by row. For each row a shape gives, analytically, the pixels that can be touched
   [nz0, nz1) and the pixels it covers completely [f0, f1); the covered span goes to the opaque fill (or to one
   gradient row) without any per-pixel work, and only the pixels in between get a coverage byte, written in
   chunks of CHUNK and handed to fill_mask / gradient_row. Coverage is computed from the pixel centre only, so it
   does not depend on the band, the clip or the chunk a pixel falls in.

   Curved edges use the distance d from the pixel centre to the circle, coverage = 0.5 - d (clamped). The distance
   comes without a square root: with q = D^2 - r^2 (D the distance to the centre), d = q / (2r) - q^2 / (8r^3) +
   O(d^3 / r^2), which is within 1/255 of the exact value for r >= 4 in the band |d| <= 0.5 where it is used
   (outside that band a comparison of D^2 with (r -+ 0.5)^2 decides 0 or 1). Smaller radii, where few pixels are
   involved, use sqrtf. Straight edges use the exact overlap of the pixel with the shape. */
#include "slate_speed.h"   /* first: the code below is built for speed */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display_font.h"
#include "display_text.h"
#include "raster.h"
#include "slate_config.h"
#include "slate_gfx.h"
#include "slate_sw.h"

#define CHUNK 128             /* coverage bytes computed at a time (on the stack) */
#define FULL_RUN_MIN 8        /* a run of fully covered pixels at least this long goes to the opaque fill */
#define FBIG 8388608.0f       /* float -> int conversions saturate here */
#define PI_F 3.14159265358979f

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline float fmx(float a, float b) { return a > b ? a : b; }
static inline float fmn(float a, float b) { return a < b ? a : b; }
static inline float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static inline unsigned div255(unsigned x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

static inline uint8_t cov8(float c)
{
    if (!(c > 0.0f)) {
        return 0;
    }
    if (c >= 1.0f) {
        return 255;
    }
    return (uint8_t)(c * 255.0f + 0.5f);
}

static inline float sat(float v) { return v < -FBIG ? -FBIG : v > FBIG ? FBIG : v; }
static inline int ifloor(float v)
{
    v = sat(v);
    int i = (int)v;
    return i - (v < (float)i);
}
static inline int iceil(float v)
{
    v = sat(v);
    int i = (int)v;
    return i + (v > (float)i);
}

/* false for inf and NaN (a bit test, which -ffinite-math-only cannot fold away) */
static inline bool isfin(float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    return (u & 0x7F800000u) != 0x7F800000u;
}

static slate_rect_t clip_band(const slate_buf_t *b, slate_rect_t c)
{
    c.x0 = imax(c.x0, b->ox);
    c.y0 = imax(c.y0, b->oy);
    c.x1 = imin(c.x1, b->ox + b->w);
    c.y1 = imin(c.y1, b->oy + b->h);
    return c;
}

/* ---- paint: where coverage goes ---- */

static const slate_gfx_ops_t *raster_ops;   /* NULL: slate_gfx() */

const slate_gfx_ops_t *slate_raster_ops(const slate_gfx_ops_t *ops)
{
    const slate_gfx_ops_t *old = raster_ops;
    raster_ops = ops;
    return old;
}

typedef struct {
    slate_buf_t *b;
    const slate_gfx_ops_t *g;
    slate_rect_t cl;                 /* clip and band, screen */
    const slate_paint_grad_t *gr;    /* NULL: solid */
    int ns;
    uint16_t c565;
    uint8_t ea;                      /* solid: colour alpha times a */
    uint8_t ga;                      /* gradient: a */
    bool opaque;                     /* a fully covered pixel is written opaque (the fill fast path is valid) */
    float gx, gy, g0;                /* linear: t = gx * px + gy * py + g0 at a pixel centre */
    float rcx, rcy, irw, irh;        /* radial: t = |((px - rcx) * irw, (py - rcy) * irh)| */
} pen_t;

/* false when nothing would be drawn. (x0, y0, x1, y1): the box the gradient spans. */
static bool pen_init(pen_t *p, slate_buf_t *b, slate_rect_t clip, slate_paint_t paint, uint8_t a, float x0, float y0,
                     float x1, float y1)
{
    if (!b || !b->px || a == 0) {
        return false;
    }
    p->b = b;
    p->g = raster_ops ? raster_ops : slate_gfx();
    p->cl = clip_band(b, clip);
    if (p->cl.x0 >= p->cl.x1 || p->cl.y0 >= p->cl.y1) {
        return false;
    }
    p->gr = paint.grad;
    if (!p->gr) {
        p->c565 = slate_rgb565(paint.argb & 0xFFFFFF);
        p->ea = (uint8_t)div255((paint.argb >> 24) * a);
        p->opaque = p->ea == 255;
        return p->ea != 0;
    }
#if !SLATE_HAS_GRADIENTS
    (void)x0, (void)y0, (void)x1, (void)y1;
    return false;   /* gradients are not built: the PHP API refuses them before this */
#else
    const slate_paint_grad_t *g = p->gr;
    if (!g->stops || g->ns <= 0) {
        return false;
    }
    p->ns = imin(g->ns, SLATE_GRADIENT_STOPS_MAX);
    p->ga = a;
    p->opaque = a == 255;
    for (int i = 0; i < p->ns; i++) {
        p->opaque = p->opaque && (g->stops[i].argb >> 24) == 255;
    }
    float w = x1 - x0, h = y1 - y0;
    if (!g->radial) {
        /* cos and sin of the angle, kept while the same angle comes back (every band, every frame) */
        static struct { bool ok; uint32_t th; float c, s; } trig;
        float th = (isfin(g->angle) ? g->angle : 0.0f) * (PI_F / 180.0f);
        uint32_t bits;
        memcpy(&bits, &th, sizeof bits);
        if (!trig.ok || trig.th != bits) {
            trig.c = cosf(th);
            trig.s = sinf(th);
            trig.th = bits;
            trig.ok = true;
        }
        float c = trig.c, s = trig.s;
        float pa = x0 * c + y0 * s, pb = x1 * c + y0 * s, pc = x0 * c + y1 * s, pd = x1 * c + y1 * s;
        float pmin = fmn(fmn(pa, pb), fmn(pc, pd)), pmax = fmx(fmx(pa, pb), fmx(pc, pd));
        float inv = pmax - pmin > 1e-6f ? 1.0f / (pmax - pmin) : 0.0f;
        p->gx = c * inv;
        p->gy = s * inv;
        p->g0 = -pmin * inv;
    } else {
        float cx = isfin(g->cx) ? g->cx : 0.5f, cy = isfin(g->cy) ? g->cy : 0.5f, r = isfin(g->r) ? g->r : 0.5f;
        p->rcx = x0 + cx * w;
        p->rcy = y0 + cy * h;
        p->irw = 1.0f / fmx(r * w, 1e-3f);
        p->irh = 1.0f / fmx(r * h, 1e-3f);
    }
    return true;
#endif
}

#if SLATE_HAS_GRADIENTS
/* Radial t at the centre of screen column X of the row whose v and v^2 are given. */
static inline float rad_t(const pen_t *p, int X, float v2)
{
    float u = ((float)X + 0.5f - p->rcx) * p->irw;
    return sqrtf(u * u + v2);
}

/* Can the screen columns [a, a + L) of the row take one linear t (within tol of the true one)? Not across the
   centre column (t has a kink there), and with the bend L^2 / 8 * t'' <= tol, t'' = v^2 du^2 / t^3 taken at the
   column nearest the centre; squared so no root is needed: (L-1)^4 v^4 du^4 <= 64 tol^2 t^6. */
static bool rad_piece_ok(const pen_t *p, int a, int L, float v2)
{
    const float tol = 1.0f / 256.0f;   /* one 8-bit level of a full-range ramp; an RGB565 step is 4 to 8 */
    float du = p->irw;
    float u0 = ((float)a + 0.5f - p->rcx) * du, u1 = ((float)(a + L - 1) + 0.5f - p->rcx) * du;
    if ((u0 < 0.0f) != (u1 < 0.0f)) {
        return false;
    }
    if (v2 == 0.0f) {
        return true;   /* |u|: linear on each side */
    }
    float un = u0 < 0.0f ? u1 : u0, w = un * un + v2;   /* t^2 at the column nearest the centre */
    float l = (float)(L - 1), kk = v2 * du * du;
    return l * l * l * l * kk * kk <= 64.0f * tol * tol * w * w * w;
}

/* Radial gradient row. t is not linear along a row, so the row is cut into pieces on a grid of screen columns
   (power-of-two lengths up to 32, aligned to their length, the longest that keeps a linear t within tol and does
   not cross the centre column); t is exact at both ends of each piece and passed anchored to screen column 0, so
   a piece cut by a segment start, a clip or a buffer edge gives the same pixels. */
static void grad_radial(const pen_t *p, int x, int y, int n, const uint8_t *cov)
{
    float v = ((float)y + 0.5f - p->rcy) * p->irh, v2 = v * v;
    int yl = y - p->b->oy;
    int X = x, end = x + n;
    slate_sw_grow_t sw;   /* the software gradient row: its per-row work once for all the pieces */
    bool soft = slate_sw_gradient(p->g);
    if (soft && !slate_sw_grow_begin(&sw, p->b, yl, p->gr->stops, p->ns)) {
        return;   /* the row is outside the buffer: gradient_row would draw nothing */
    }
    while (X < end) {
        int L = 32, a = X & -32;
        while (L > 1 && !rad_piece_ok(p, a, L, v2)) {
            L >>= 1;
            a = X & -L;
        }
        float ta = rad_t(p, a, v2);
        float dt = L > 1 ? (rad_t(p, a + L - 1, v2) - ta) / (float)(L - 1) : 0.0f;
        int e = imin(a + L, end);
        float t0 = ta - (float)a * dt;
        if (soft) {
            slate_sw_grow_piece(&sw, X - p->b->ox, e - X, t0, dt, cov ? cov + (X - x) : NULL);
        } else {
            p->g->gradient_row(p->b, X - p->b->ox, yl, e - X, t0, dt, p->gr->stops, p->ns, cov ? cov + (X - x) : NULL);
        }
        X = e;
    }
}

/* n gradient pixels from screen (x, y); cov may be NULL (all covered); a is already folded into cov. */
static void grad_any(const pen_t *p, int x, int y, int n, const uint8_t *cov)
{
    if (p->gr->radial) {
        grad_radial(p, x, y, n, cov);
    } else {   /* t0: t at screen column 0 of the row, the same for every segment of the row */
        float t0 = p->gy * ((float)y + 0.5f) + p->g0 + p->gx * 0.5f;
        p->g->gradient_row(p->b, x - p->b->ox, y - p->b->oy, n, t0, p->gx, p->gr->stops, p->ns, cov);
    }
}

static void grad_row(const pen_t *p, int x, int y, int n, const uint8_t *cov)
{
    if (p->ga == 255) {
        grad_any(p, x, y, n, cov);
        return;
    }
    uint8_t tmp[CHUNK];
    for (int k = 0; k < n; k += CHUNK) {
        int m = imin(CHUNK, n - k);
        for (int i = 0; i < m; i++) {
            tmp[i] = (uint8_t)(cov ? div255((unsigned)cov[k + i] * p->ga) : p->ga);
        }
        grad_any(p, x + k, y, m, tmp);
    }
}
#endif

/* n fully covered pixels from screen (x, y), inside p->cl. */
static void emit_full(const pen_t *p, int x, int y, int n)
{
#if SLATE_HAS_GRADIENTS
    if (p->gr) {
        grad_row(p, x, y, n, NULL);
        return;
    }
#endif
    int xl = x - p->b->ox, yl = y - p->b->oy;
    slate_rect_t r = { xl, yl, xl + n, yl + 1 };
    if (p->ea == 255) {
        p->g->fill(p->b, r, p->c565);
    } else {
        p->g->fill_a(p->b, r, p->c565, p->ea);
    }
}

/* n pixels from screen (x, y) with coverage cov, inside p->cl. Long fully covered runs of an opaque solid go to
   the fill; everything else to one fill_mask per stretch (zero runs skipped). */
static void emit_cov(const pen_t *p, int x, int y, int n, const uint8_t *cov)
{
    while (n > 0 && cov[0] == 0) {
        x++, cov++, n--;
    }
    while (n > 0 && cov[n - 1] == 0) {
        n--;
    }
    if (n <= 0) {
        return;
    }
#if SLATE_HAS_GRADIENTS
    if (p->gr) {
        grad_row(p, x, y, n, cov);
        return;
    }
#endif
    int xl = x - p->b->ox, yl = y - p->b->oy;
    bool fast = p->ea == 255;
    int i = 0;
    while (i < n) {
        while (i < n && cov[i] == 0) {
            i++;
        }
        if (i >= n) {
            break;
        }
        int j = i, full_end = -1;
        while (j < n && cov[j] != 0) {
            if (fast && cov[j] == 255) {
                int k = j;
                while (k < n && cov[k] == 255) {
                    k++;
                }
                if (k - j >= FULL_RUN_MIN) {
                    full_end = k;
                    break;
                }
                j = k;
            } else {
                j++;
            }
        }
        if (j > i) {
            p->g->fill_mask(p->b, (slate_rect_t){ xl + i, yl, xl + j, yl + 1 }, p->c565, p->ea, cov + i, n);
        }
        if (full_end >= 0) {
            p->g->fill(p->b, (slate_rect_t){ xl + j, yl, xl + full_end, yl + 1 }, p->c565);
            j = full_end;
        }
        i = j;
    }
}

/* Coverage of [x, x + n) of the current row of a shape. */
typedef void (*seg_fn)(const void *shape, const void *row, int x, int n, uint8_t *out);

static void emit_seg(const pen_t *p, int y, int xa, int xb, seg_fn fn, const void *shape, const void *row)
{
    uint8_t cov[CHUNK];
    for (int x = xa; x < xb; x += CHUNK) {
        int n = imin(CHUNK, xb - x);
        fn(shape, row, x, n, cov);
        emit_cov(p, x, y, n, cov);
    }
}

/* One row: [nz0, nz1) may be touched, [f0, f1) is fully covered (empty when f1 <= f0). */
static void emit_shape(const pen_t *p, int y, int nz0, int nz1, int f0, int f1, seg_fn fn, const void *shape,
                       const void *row)
{
    nz0 = imax(nz0, p->cl.x0);
    nz1 = imin(nz1, p->cl.x1);
    if (nz0 >= nz1) {
        return;
    }
    if (f1 <= f0) {
        f0 = f1 = nz1;
    }
    f0 = imin(imax(f0, nz0), nz1);
    f1 = imin(imax(f1, f0), nz1);
    emit_seg(p, y, nz0, f0, fn, shape, row);
    if (f1 > f0) {
        emit_full(p, f0, y, f1 - f0);
    }
    emit_seg(p, y, f1, nz1, fn, shape, row);
}

/* ---- disc coverage ---- */

typedef struct { float r, r2, in2, out2, k1, k2; bool series; } circ_t;

static void circ_init(circ_t *c, float r)
{
    c->r = r;
    c->r2 = r * r;
    c->out2 = r > 0.0f ? (r + 0.5f) * (r + 0.5f) : -1.0f;   /* D^2 >= out2: coverage 0 */
    c->in2 = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : -1.0f;    /* D^2 <= in2: coverage 1 */
    c->series = r >= 4.0f;
    c->k1 = c->series ? 0.5f / r : 0.0f;
    c->k2 = c->series ? 0.125f / (r * r * r) : 0.0f;
}

/* Coverage of a pixel whose centre is at squared distance D2 from the centre of the disc. */
static inline float circ_cov(const circ_t *c, float D2)
{
    if (D2 >= c->out2) {
        return 0.0f;
    }
    if (D2 <= c->in2) {
        return 1.0f;
    }
    if (c->series) {
        float q = D2 - c->r2;
        return clamp01(0.5f - q * (c->k1 - q * c->k2));
    }
    float D = sqrtf(D2);   /* small disc: overlap of [D - 0.5, D + 0.5] with [-r, r] */
    return clamp01(fmn(c->r, D + 0.5f) - fmx(-c->r, D - 0.5f));
}

/* ---- rounded rectangle (and disc) ---- */

typedef struct { float x0, y0, x1, y1, r; circ_t c; } rr_t;
typedef struct { float cyo, dy2, ccl, ccr; bool corner; int nz0, nz1, f0, f1; } rr_row_t;

static bool rr_init(rr_t *s, float x0, float y0, float x1, float y1, float r)
{
    if (!(x1 > x0 && y1 > y0)) {
        return false;
    }
    float m = fmn(x1 - x0, y1 - y0) * 0.5f;
    r = r > m ? m : r > 0.0f ? r : 0.0f;
    s->x0 = x0, s->y0 = y0, s->x1 = x1, s->y1 = y1, s->r = r;
    circ_init(&s->c, r);
    return true;
}

static bool rr_row(const rr_t *s, int y, rr_row_t *o)
{
    float fy = (float)y, py = fy + 0.5f, r = s->r;
    float cyo = fmn(fy + 1.0f, s->y1) - fmx(fy, s->y0);   /* vertical overlap of the row with the box */
    if (cyo <= 0.0f) {
        return false;
    }
    o->cyo = cyo > 1.0f ? 1.0f : cyo;
    o->ccl = s->x0 + r;
    o->ccr = s->x1 - r;
    o->corner = r > 0.0f && (py < s->y0 + r || py > s->y1 - r);
    int bx0 = ifloor(s->x0), bx1 = iceil(s->x1);
    o->nz0 = bx0;
    o->nz1 = bx1;
    o->f0 = o->f1 = 0;
    if (o->corner) {
        float dy = py < s->y0 + r ? py - (s->y0 + r) : py - (s->y1 - r);
        o->dy2 = dy * dy;
        float e2 = (r + 0.5f) * (r + 0.5f) - o->dy2, eo = e2 > 0.0f ? sqrtf(e2) : 0.0f;
        o->nz0 = imax(bx0, ifloor(o->ccl - eo - 0.5f));
        o->nz1 = imin(bx1, ifloor(o->ccr + eo - 0.5f) + 1);
        if (o->cyo >= 1.0f && r >= 0.5f) {
            float i2 = (r - 0.5f) * (r - 0.5f) - o->dy2, ei = i2 > 0.0f ? sqrtf(i2) : 0.0f;
            o->f0 = iceil(o->ccl - ei - 0.5f);
            o->f1 = ifloor(o->ccr + ei - 0.5f) + 1;
        }
    } else if (o->cyo >= 1.0f) {
        o->f0 = iceil(s->x0);
        o->f1 = ifloor(s->x1);
    }
    return true;
}

static inline float rr_px(const rr_t *s, const rr_row_t *o, int x)
{
    float px = (float)x + 0.5f;
    if (o->corner) {
        if (px < o->ccl) {
            float dx = px - o->ccl;
            return circ_cov(&s->c, dx * dx + o->dy2);
        }
        if (px > o->ccr) {
            float dx = px - o->ccr;
            return circ_cov(&s->c, dx * dx + o->dy2);
        }
    }
    return o->cyo * clamp01(fmn((float)x + 1.0f, s->x1) - fmx((float)x, s->x0));
}

static void rr_seg(const void *shape, const void *row, int x, int n, uint8_t *out)
{
    for (int i = 0; i < n; i++) {
        out[i] = cov8(rr_px(shape, row, x + i));
    }
}

/* The border band of one row: outer shape O minus inner shape I. under: the fill (opaque) is drawn over it
   afterwards, so the band takes the whole outer coverage wherever the fill is not complete, and the inner edge
   has no seam; otherwise the band is cov(O) - cov(I). */
typedef struct { const rr_t *o; const rr_row_t *ro; const rr_t *i; const rr_row_t *ri; bool under; } ring_t;

static void ring_seg(const void *shape, const void *row, int x, int n, uint8_t *out)
{
    const ring_t *k = shape;
    (void)row;
    for (int j = 0; j < n; j++) {
        int xx = x + j;
        unsigned co = xx >= k->ro->f0 && xx < k->ro->f1 ? 255 : cov8(rr_px(k->o, k->ro, xx));
        unsigned ci = 0;
        if (k->i && xx >= k->ri->nz0 && xx < k->ri->nz1) {
            ci = xx >= k->ri->f0 && xx < k->ri->f1 ? 255 : cov8(rr_px(k->i, k->ri, xx));
        }
        out[j] = (uint8_t)(k->under ? (ci == 255 ? 0 : co) : (co > ci ? co - ci : 0));
    }
}

static void draw_rr(slate_buf_t *b, slate_rect_t clip, const rr_t *O, slate_paint_t fill, float border,
                    uint32_t border_argb, uint8_t a)
{
    pen_t pf, pb;
    bool has_fill = pen_init(&pf, b, clip, fill, a, O->x0, O->y0, O->x1, O->y1);
    bool has_border = isfin(border) && border > 0.0f
                      && pen_init(&pb, b, clip, (slate_paint_t){ border_argb, NULL }, a, O->x0, O->y0, O->x1, O->y1);
    if (!has_fill && !has_border) {
        return;
    }
    const pen_t *pc = has_fill ? &pf : &pb;
    rr_t I;
    bool has_inner = has_border && rr_init(&I, O->x0 + border, O->y0 + border, O->x1 - border, O->y1 - border,
                                           O->r - border);
    int y0 = imax(pc->cl.y0, ifloor(O->y0)), y1 = imin(pc->cl.y1, iceil(O->y1));
    for (int y = y0; y < y1; y++) {
        rr_row_t ro, ri;
        if (!rr_row(O, y, &ro)) {
            continue;
        }
        if (!has_border) {
            emit_shape(&pf, y, ro.nz0, ro.nz1, ro.f0, ro.f1, rr_seg, O, &ro);
            continue;
        }
        bool row_i = has_inner && rr_row(&I, y, &ri);
        bool under = has_fill && pf.opaque && row_i;
        if (has_fill && row_i && !under) {
            emit_shape(&pf, y, ri.nz0, ri.nz1, ri.f0, ri.f1, rr_seg, &I, &ri);
        }
        ring_t k = { O, &ro, row_i ? &I : NULL, &ri, under };
        int h0 = ro.nz1, h1 = ro.nz1;
        if (row_i && ri.f1 > ri.f0) {   /* the band is empty where the inner shape is complete */
            h0 = ri.f0;
            h1 = ri.f1;
        }
        emit_shape(&pb, y, ro.nz0, imin(h0, ro.nz1), 0, 0, ring_seg, &k, NULL);
        emit_shape(&pb, y, imax(h1, ro.nz0), ro.nz1, 0, 0, ring_seg, &k, NULL);
        if (under) {
            emit_shape(&pf, y, ri.nz0, ri.nz1, ri.f0, ri.f1, rr_seg, &I, &ri);
        }
    }
}

void slate_draw_roundrect(slate_buf_t *b, slate_rect_t clip, float x, float y, float w, float h, float radius,
                          slate_paint_t fill, float border, uint32_t border_argb, uint8_t a)
{
    if (!isfin(x) || !isfin(y) || !isfin(w) || !isfin(h) || !isfin(radius)) {
        return;
    }
    rr_t O;
    if (rr_init(&O, x, y, x + w, y + h, radius)) {
        draw_rr(b, clip, &O, fill, border, border_argb, a);
    }
}

void slate_draw_circle(slate_buf_t *b, slate_rect_t clip, float cx, float cy, float r, slate_paint_t fill,
                       float border, uint32_t border_argb, uint8_t a)
{
    if (!isfin(cx) || !isfin(cy) || !isfin(r)) {
        return;
    }
    rr_t O;
    if (r > 0.0f && rr_init(&O, cx - r, cy - r, cx + r, cy + r, r)) {
        draw_rr(b, clip, &O, fill, border, border_argb, a);
    }
}

/* ---- arc ---- */

/* One bound of the sure columns of a row (arc_row): the columns where alpha * dy + b * dx >= t (dx = x + 0.5 - cx,
   dy the row's), as a subset (sure) or a superset of them. With b away from 0 it is the half line from the column
   v = base + slope * dy: x >= ceil(v) + adj (ARC_LO) or x < floor(v) + 1 + adj (ARC_HI). With b = 0 it holds on
   the whole row or on none of it, by alpha * dy >= t (ARC_ROW); with b too small to divide by, a sure set is
   empty (ARC_NONE) and a superset is every column (ARC_ALL). */
enum { ARC_ALL, ARC_NONE, ARC_LO, ARC_HI, ARC_ROW };
typedef struct { uint8_t kind; int8_t adj; float base, slope, alpha, t; } arc_bound_t;

typedef struct {
    float cx, cy, hw;
    circ_t o, i, cap;
    bool full, round, wide;           /* wide: sweep > 180, the wedge is the union of two half planes */
    float sx, sy, ex, ey;             /* unit vectors of the start and end rays */
    float mx, my;                     /* unit vector of the sweep's bisector */
    float p0x, p0y, p1x, p1y;         /* cap centres */
    arc_bound_t in[3], out[3];        /* sure inside the sweep, possibly not outside it (arc_row) */
} arc_t;
/* A row of an arc. The per-pixel coverage (arc_px) is only needed near the ring's edges, the ends and the caps:
   arc_row() finds, from the row's geometry alone, the columns where the pixel is certainly complete (ring complete
   and inside the sweep) and those where it is certainly empty (outside the sweep, away from the caps), each with
   at least half a pixel of margin and a column more, so that arc_px would give exactly 1 or 0 there. Those
   columns are written without arc_px, the others go through it: the same bytes either way. */
typedef struct {
    float py, dy, dy2;
    bool cls;    /* the columns below are set; false: every pixel goes through arc_px */
    int rf[4];   /* the ring is complete in [rf[0], rf[1]) and [rf[2], rf[3]) */
    int in0, in1;     /* narrow: inside the sweep in [in0, in1); wide: inside outside [in0, in1) */
    int out0, out1;   /* narrow: outside the sweep outside [out0, out1); wide: outside in [out0, out1) */
    int cap[4];       /* round caps: columns [cap[0], cap[1]) and [cap[2], cap[3]) may take a cap */
} arc_row_t;

static inline float arc_px(const arc_t *s, const arc_row_t *o, int x)
{
    float px = (float)x + 0.5f, dx = px - s->cx, D2 = dx * dx + o->dy2;
    float c = fmn(circ_cov(&s->o, D2), 1.0f - circ_cov(&s->i, D2));
    if (c <= 0.0f || s->full) {
        return c;
    }
    float c1 = clamp01(0.5f + (s->sx * o->dy - s->sy * dx));   /* clockwise of the start ray */
    float c2 = clamp01(0.5f + (dx * s->ey - o->dy * s->ex));   /* anticlockwise of the end ray */
    /* The two half planes run through the centre, so on the far side of it their lines are as close as on the
       near side: for a nearly full arc they would cut a notch opposite the gap, for a tiny one leave a sliver
       opposite it. The bisector settles that far side: at least half a pixel past the centre away from the gap
       (wide) a pixel is fully inside the sweep, and behind the centre of a sweep <= 180 it is outside. */
    float side = dx * s->mx + o->dy * s->my;
    float w = s->wide ? (side >= 0.5f ? 1.0f : fmx(c1, c2)) : (side <= -0.5f ? 0.0f : fmn(c1, c2));
    c = fmn(c, w);
    if (s->round && c < 1.0f) {
        float lim = s->hw + 1.0f;
        float ax = px - s->p0x, ay = o->py - s->p0y;
        if (ax < lim && ax > -lim && ay < lim && ay > -lim) {
            c = fmx(c, circ_cov(&s->cap, ax * ax + ay * ay));
        }
        ax = px - s->p1x;
        ay = o->py - s->p1y;
        if (ax < lim && ax > -lim && ay < lim && ay > -lim) {
            c = fmx(c, circ_cov(&s->cap, ax * ax + ay * ay));
        }
    }
    return c;
}

#define COL_ALL 0x20000000   /* "every column" (columns are within +-FBIG) */

static inline bool in_cols(int x, int a, int b) { return x >= a && x < b; }


/* The bound of the columns where alpha * dy + b * dx >= t (see arc_bound_t); sure: a subset, one column less,
   otherwise a superset, one column more. */
static arc_bound_t arc_bound(float alpha, float b, float t, float cx, bool sure)
{
    arc_bound_t k = { ARC_ROW, 0, 0.0f, 0.0f, alpha, t };
    if (b > 1e-6f || b < -1e-6f) {
        float ib = 1.0f / b;   /* the column where alpha * dy + b * dx = t */
        k.kind = b > 0.0f ? ARC_LO : ARC_HI;
        k.adj = (int8_t)(b > 0.0f ? (sure ? 1 : -1) : (sure ? -1 : 1));
        k.base = t * ib + cx - 0.5f;
        k.slope = -alpha * ib;
    } else if (b != 0.0f) {
        k.kind = sure ? ARC_NONE : ARC_ALL;
    }
    return k;
}

/* [*lo, *hi) cut by the columns of bound k on the row at dy. */
static inline void bound_cols(const arc_bound_t *k, float dy, int *lo, int *hi)
{
    switch (k->kind) {
    case ARC_LO:
        *lo = imax(*lo, iceil(k->base + k->slope * dy) + k->adj);
        break;
    case ARC_HI:
        *hi = imin(*hi, ifloor(k->base + k->slope * dy) + 1 + k->adj);
        break;
    case ARC_NONE:
        *hi = *lo;
        break;
    case ARC_ROW:
        if (!(k->alpha * dy >= k->t)) {
            *hi = *lo;
        }
        break;
    default:
        break;
    }
}

/* sqrt(v) for v > 0 within a few 1e-7 relative, multiplies only (sqrtf is a library call on some targets): for
   the bounds of the rows, which keep a margin far larger than that. */
static inline float sqrt_near(float v)
{
    union { float f; uint32_t u; } c = { v };
    c.u = 0x5F3759DFu - (c.u >> 1);
    float y = c.f, h = 0.5f * v;
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    y = y * (1.5f - h * y * y);
    return v * y;
}

/* Margin of the sure columns: half a pixel on the distance to an edge (the half planes have unit normals). */
#define ARC_SURE 0.5f
/* A row narrower than this (within the clip) goes through arc_px pixel by pixel: finding its sure columns would
   cost more than it saves. */
#define ARC_CLS_MIN 12

/* The sure bounds of an arc's rows, once per arc (arc_geo). c1 = 0.5 + L1, L1 = sx dy - sy dx; c2 = 0.5 + L2,
   L2 = ey dx - ex dy; side = mx dx + my dy (arc_px). Narrow (sweep <= 180): inside where side > -0.5, c1 = 1 and
   c2 = 1; outside wherever side <= -0.5, c1 = 0 or c2 = 0, so possibly not outside only where all three fail.
   Wide: inside wherever side >= 0.5, c1 = 1 or c2 = 1, so possibly not inside only where all three fail (the
   interval in[] then holds the columns that may not be inside); outside where side < 0.5, c1 = 0 and c2 = 0. */
static void arc_bounds(arc_t *s)
{
    float cx = s->cx, m = ARC_SURE;
    if (!s->wide) {
        s->in[0] = arc_bound(s->my, s->mx, -0.5f + m, cx, true);
        s->in[1] = arc_bound(s->sx, -s->sy, 0.5f + m, cx, true);
        s->in[2] = arc_bound(-s->ex, s->ey, 0.5f + m, cx, true);
        s->out[0] = arc_bound(s->my, s->mx, -0.5f - m, cx, false);
        s->out[1] = arc_bound(s->sx, -s->sy, -0.5f - m, cx, false);
        s->out[2] = arc_bound(-s->ex, s->ey, -0.5f - m, cx, false);
    } else {
        s->in[0] = arc_bound(-s->my, -s->mx, -0.5f - m, cx, false);
        s->in[1] = arc_bound(-s->sx, s->sy, -0.5f - m, cx, false);
        s->in[2] = arc_bound(s->ex, -s->ey, -0.5f - m, cx, false);
        s->out[0] = arc_bound(-s->my, -s->mx, -0.5f + m, cx, true);
        s->out[1] = arc_bound(-s->sx, s->sy, 0.5f + m, cx, true);
        s->out[2] = arc_bound(s->ex, -s->ey, 0.5f + m, cx, true);
    }
}

/* The row at y; cls: find its sure columns too. */
static void arc_row(const arc_t *s, int y, bool cls, arc_row_t *o)
{
    o->py = (float)y + 0.5f;
    o->dy = o->py - s->cy;
    o->dy2 = o->dy * o->dy;
    o->cls = cls;
    if (!cls) {
        return;
    }
    float cx = s->cx, dy = o->dy;
    /* the ring complete: D^2 <= (ro - 0.5)^2 (outer coverage 1) and D^2 >= (ri + 0.5)^2 (inner coverage 0) */
    o->rf[0] = o->rf[1] = o->rf[2] = o->rf[3] = 0;
    float oi = s->o.in2 - o->dy2;
    if (oi > 0.0f) {
        float ea = sqrt_near(oi) - ARC_SURE;
        float ii = s->i.out2 - o->dy2, eb = ii > 0.0f ? sqrt_near(ii) + ARC_SURE : 0.0f;
        if (ea > eb) {
            o->rf[0] = iceil(cx - 0.5f - ea) + 1;
            o->rf[1] = ifloor(cx - 0.5f - eb);
            o->rf[2] = iceil(cx - 0.5f + eb) + 1;
            o->rf[3] = ifloor(cx - 0.5f + ea);
        }
    }
    if (s->full) {
        return;
    }
    o->in0 = o->out0 = -COL_ALL;
    o->in1 = o->out1 = COL_ALL;
    for (int k = 0; k < 3; k++) {
        bound_cols(&s->in[k], dy, &o->in0, &o->in1);
        bound_cols(&s->out[k], dy, &o->out0, &o->out1);
    }
    o->cap[0] = o->cap[1] = o->cap[2] = o->cap[3] = 0;
    if (s->round) {   /* arc_px looks at a cap within lim of its centre on both axes */
        float lim = s->hw + 1.0f;
        if (fabsf(o->py - s->p0y) < lim + 1.0f) {
            o->cap[0] = ifloor(s->p0x - lim - 0.5f) - 1;
            o->cap[1] = iceil(s->p0x + lim - 0.5f) + 2;
        }
        if (fabsf(o->py - s->p1y) < lim + 1.0f) {
            o->cap[2] = ifloor(s->p1x - lim - 0.5f) - 1;
            o->cap[3] = iceil(s->p1x + lim - 0.5f) + 2;
        }
    }
}

/* ARC_RING: inside the sweep, away from the caps: arc_px is the ring's coverage alone. */
enum { ARC_PX, ARC_ZERO, ARC_FULL, ARC_RING };

static int arc_state(const arc_t *s, const arc_row_t *o, int x)
{
    bool ring = in_cols(x, o->rf[0], o->rf[1]) || in_cols(x, o->rf[2], o->rf[3]);
    if (s->full) {
        return ring ? ARC_FULL : ARC_RING;
    }
    bool in = in_cols(x, o->in0, o->in1) != s->wide;
    if (ring && in) {
        return ARC_FULL;
    }
    bool cap = in_cols(x, o->cap[0], o->cap[1]) || in_cols(x, o->cap[2], o->cap[3]);
    if (in && !cap) {
        return ARC_RING;
    }
    if (!cap && in_cols(x, o->out0, o->out1) == s->wide) {
        return ARC_ZERO;
    }
    return ARC_PX;
}

/* arc_px of a pixel that is inside the sweep and away from the caps */
static inline float arc_ring_px(const arc_t *s, const arc_row_t *o, int x)
{
    float px = (float)x + 0.5f, dx = px - s->cx, D2 = dx * dx + o->dy2;
    return fmn(circ_cov(&s->o, D2), 1.0f - circ_cov(&s->i, D2));
}

/* arc_px of the columns [x, x + n): its only caller, so that arc_px is inlined in the loop */
static void arc_px_run(const arc_t *s, const arc_row_t *o, int x, int n, uint8_t *out)
{
    for (int i = 0; i < n; i++) {
        out[i] = cov8(arc_px(s, o, x + i));
    }
}

static void arc_seg(const void *shape, const void *row, int x, int n, uint8_t *out)
{
    const arc_t *s = shape;
    const arc_row_t *o = row;
    if (!o->cls) {
        arc_px_run(s, o, x, n, out);
        return;
    }
    /* the classes only change at these columns: between two of them one class holds */
    int cut[14] = { o->rf[0], o->rf[1], o->rf[2], o->rf[3], o->in0, o->in1, o->out0, o->out1,
                    o->cap[0], o->cap[1], o->cap[2], o->cap[3], x + n, x + n };
    int nc = 0;
    for (int k = 0; k < 14; k++) {
        int v = cut[k];
        if (v > x && v <= x + n) {
            int j = nc++;
            for (; j > 0 && cut[j - 1] > v; j--) {
                cut[j] = cut[j - 1];
            }
            cut[j] = v;
        }
    }
    int at = x;
    for (int k = 0; k < nc && at < x + n; k++) {
        int e = cut[k];
        if (e <= at) {
            continue;
        }
        switch (arc_state(s, o, at)) {
        case ARC_FULL:
            memset(out + (at - x), 255, (size_t)(e - at));
            break;
        case ARC_ZERO:
            memset(out + (at - x), 0, (size_t)(e - at));
            break;
        case ARC_RING:
            for (int i = at; i < e; i++) {
                out[i - x] = cov8(arc_ring_px(s, o, i));
            }
            break;
        default:
            arc_px_run(s, o, at, e - at, out + (at - x));
            break;
        }
        at = e;
    }
}

/* Is angle a (degrees) within the sweep that starts at from and is sweep long? */
static bool in_sweep(float a, float from, float sweep)
{
    float d = fmodf(a - from, 360.0f);
    if (d < 0.0f) {
        d += 360.0f;
    }
    return d <= sweep;
}

/* The geometry of an arc: s and its bounding box (bx0, by0, bx1, by1). false: nothing to draw (sweep 0).
   s.in and s.out are set later, by the first row that needs them (bounds true). */
typedef struct { arc_t s; float bx0, by0, bx1, by1; bool bounds; } arc_geo_t;

static bool arc_geo(arc_geo_t *g, float cx, float cy, float r, float from_deg, float to_deg, float width,
                    slate_caps_t caps)
{
    arc_t s;
    float hw = width * 0.5f, ro = r + hw, ri = r - hw;
    float span = to_deg - from_deg, sweep = 360.0f;
    s.full = span >= 360.0f || span <= -360.0f;
    if (!s.full) {
        sweep = fmodf(span, 360.0f);
        if (sweep < 0.0f) {
            sweep += 360.0f;
        }
        if (sweep == 0.0f) {
            return false;
        }
    }
    s.cx = cx;
    s.cy = cy;
    s.hw = hw;
    circ_init(&s.o, ro);
    circ_init(&s.i, ri);
    circ_init(&s.cap, hw);
    s.round = caps == SLATE_CAPS_ROUND;
    s.wide = sweep > 180.0f;
    float a0 = from_deg * (PI_F / 180.0f), a1 = (from_deg + sweep) * (PI_F / 180.0f);
    s.sx = cosf(a0);
    s.sy = sinf(a0);
    s.ex = cosf(a1);
    s.ey = sinf(a1);
    s.p0x = cx + r * s.sx;
    s.p0y = cy + r * s.sy;
    s.p1x = cx + r * s.ex;
    s.p1y = cy + r * s.ey;
    float am = (from_deg + sweep * 0.5f) * (PI_F / 180.0f);
    s.mx = cosf(am);
    s.my = sinf(am);

    /* bounding box: the whole ring, or for a part of it the two ends and the axis points inside the sweep */
    float bx0 = cx - ro, by0 = cy - ro, bx1 = cx + ro, by1 = cy + ro;
    if (!s.full) {
        float e = hw + 1.0f;
        bx0 = fmn(s.p0x, s.p1x) - e;
        bx1 = fmx(s.p0x, s.p1x) + e;
        by0 = fmn(s.p0y, s.p1y) - e;
        by1 = fmx(s.p0y, s.p1y) + e;
        if (in_sweep(0.0f, from_deg, sweep)) bx1 = cx + ro + 1.0f;
        if (in_sweep(90.0f, from_deg, sweep)) by1 = cy + ro + 1.0f;
        if (in_sweep(180.0f, from_deg, sweep)) bx0 = cx - ro - 1.0f;
        if (in_sweep(270.0f, from_deg, sweep)) by0 = cy - ro - 1.0f;
    }
    *g = (arc_geo_t){ s, bx0, by0, bx1, by1, false };
    return true;
}

/* The last arcs' geometry, by their parameters' bits: an arc is drawn once per band it crosses, and again in the
   next frames while it does not change, and its geometry costs several trigonometric calls. Only the compositor
   task draws (raster.h), so one memo serves. */
#define ARC_MEMO 4
static struct {
    uint32_t key[7];
    bool used, drawn;
    arc_geo_t g;
} arc_memo[ARC_MEMO];
static unsigned arc_memo_next;

static arc_geo_t *arc_geo_memo(float cx, float cy, float r, float from_deg, float to_deg, float width,
                                     slate_caps_t caps)
{
    uint32_t key[7];
    const float in[6] = { cx, cy, r, from_deg, to_deg, width };
    memcpy(key, in, sizeof in);
    key[6] = (uint32_t)caps;
    for (int i = 0; i < ARC_MEMO; i++) {
        if (arc_memo[i].used && memcmp(arc_memo[i].key, key, sizeof key) == 0) {
            return arc_memo[i].drawn ? &arc_memo[i].g : NULL;
        }
    }
    unsigned k = arc_memo_next++ % ARC_MEMO;
    memcpy(arc_memo[k].key, key, sizeof key);
    arc_memo[k].used = true;
    arc_memo[k].drawn = arc_geo(&arc_memo[k].g, cx, cy, r, from_deg, to_deg, width, caps);
    return arc_memo[k].drawn ? &arc_memo[k].g : NULL;
}

void slate_draw_arc(slate_buf_t *b, slate_rect_t clip, float cx, float cy, float r, float from_deg, float to_deg,
                    float width, slate_caps_t caps, slate_paint_t paint, uint8_t a)
{
    if (!isfin(cx) || !isfin(cy) || !isfin(r) || !isfin(from_deg) || !isfin(to_deg) || !isfin(width)
        || !(width > 0.0f) || !(r > 0.0f)) {
        return;
    }
    arc_geo_t *g = arc_geo_memo(cx, cy, r, from_deg, to_deg, width, caps);
    if (!g) {
        return;
    }
    float hw = width * 0.5f, ro = r + hw, ri = r - hw;
    pen_t p;
    if (!pen_init(&p, b, clip, paint, a, cx - ro, cy - ro, cx + ro, cy + ro)) {
        return;
    }
    arc_t s = g->s;
    float bx0 = g->bx0, by0 = g->by0, bx1 = g->bx1, by1 = g->by1;
    int y0 = imax(p.cl.y0, ifloor(fmx(by0, cy - ro - 1.0f)));
    int y1 = imin(p.cl.y1, iceil(fmn(by1, cy + ro + 1.0f)));
    int xb0 = ifloor(bx0), xb1 = iceil(bx1);
    float o2 = (ro + 0.5f) * (ro + 0.5f), h2 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) : -1.0f;
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy, e2 = o2 - dy * dy;
        if (e2 <= 0.0f) {
            continue;
        }
        float eo = sqrtf(e2);
        int nz0 = imax(xb0, ifloor(cx - eo - 0.5f)), nz1 = imin(xb1, ifloor(cx + eo - 0.5f) + 1);
        float i2 = h2 - dy * dy;
        int g0 = nz1, g1 = nz1;   /* the hole, where the inner disc covers the pixel fully, is skipped */
        if (i2 > 0.0f) {
            float hi = sqrtf(i2);
            g0 = iceil(cx - hi - 0.5f);
            g1 = ifloor(cx + hi - 0.5f) + 1;
        }
        int c0 = imax(nz0, p.cl.x0), c1 = imin(nz1, p.cl.x1);   /* the pixels of the row inside the clip */
        int n = imax(0, imin(c1, g0) - c0) + imax(0, c1 - imax(c0, g1));
        if (n <= 0) {
            continue;
        }
        bool cls = n >= ARC_CLS_MIN;
        if (cls && !g->bounds && !s.full) {
            arc_bounds(&g->s);
            g->bounds = true;
            s = g->s;
        }
        arc_row_t o;
        arc_row(&s, y, cls, &o);
        if (i2 > 0.0f) {
            emit_shape(&p, y, nz0, imin(nz1, g0), 0, 0, arc_seg, &s, &o);
            emit_shape(&p, y, imax(nz0, g1), nz1, 0, 0, arc_seg, &s, &o);
        } else {
            emit_shape(&p, y, nz0, nz1, 0, 0, arc_seg, &s, &o);
        }
    }
}

/* ---- line ---- */

typedef struct { float X0, Y0, ux, uy, nx, ny, L, hw; bool round; circ_t cap; } ln_t;
typedef struct { float cs, ca; } ln_row_t;   /* s = nx * px + cs, a = ux * px + ca on this row */

static inline float ln_px(const ln_t *s, const ln_row_t *o, int x)
{
    float px = (float)x + 0.5f;
    float d = s->nx * px + o->cs, al = s->ux * px + o->ca;
    float cs = clamp01(fmn(s->hw, d + 0.5f) - fmx(-s->hw, d - 0.5f));
    if (!s->round) {
        return cs * clamp01(fmn(s->L, al + 0.5f) - fmx(0.0f, al - 0.5f));
    }
    if (al >= 0.0f && al <= s->L) {
        return cs;
    }
    float da = al < 0.0f ? al : al - s->L;
    return circ_cov(&s->cap, da * da + d * d);
}

static void ln_seg(const void *shape, const void *row, int x, int n, uint8_t *out)
{
    for (int i = 0; i < n; i++) {
        out[i] = cov8(ln_px(shape, row, x + i));
    }
}

/* The open interval of px where lo < k * px + c < hi; false when empty. ik = 1 / k (0 when k is about 0). */
static bool lin_range(float k, float ik, float c, float lo, float hi, float *a, float *b)
{
    if (k > -1e-7f && k < 1e-7f) {
        *a = -FBIG;
        *b = FBIG;
        return c > lo && c < hi;
    }
    float p = (lo - c) * ik, q = (hi - c) * ik;
    *a = fmn(p, q);
    *b = fmx(p, q);
    return true;
}

void slate_draw_line(slate_buf_t *b, slate_rect_t clip, float x0, float y0, float x1, float y1, float width,
                     slate_caps_t caps, uint32_t argb, uint8_t a)
{
    if (!isfin(x0) || !isfin(y0) || !isfin(x1) || !isfin(y1) || !isfin(width) || !(width > 0.0f)) {
        return;
    }
    ln_t s;
    s.X0 = x0 + 0.5f;   /* end points are pixel centres */
    s.Y0 = y0 + 0.5f;
    float dx = x1 - x0, dy = y1 - y0;
    s.L = sqrtf(dx * dx + dy * dy);
    s.ux = s.L > 0.0f ? dx / s.L : 1.0f;
    s.uy = s.L > 0.0f ? dy / s.L : 0.0f;
    s.nx = -s.uy;
    s.ny = s.ux;
    s.hw = width * 0.5f;
    s.round = caps == SLATE_CAPS_ROUND;
    circ_init(&s.cap, s.hw);
    if (!s.round && s.L <= 0.0f) {
        return;
    }
    float ext = s.round ? s.hw : 0.0f, m = s.hw + ext + 1.0f;
    float bx0 = fmn(s.X0, s.X0 + dx) - m, bx1 = fmx(s.X0, s.X0 + dx) + m;
    float by0 = fmn(s.Y0, s.Y0 + dy) - m, by1 = fmx(s.Y0, s.Y0 + dy) + m;
    pen_t p;
    if (!pen_init(&p, b, clip, (slate_paint_t){ argb, NULL }, a, bx0, by0, bx1, by1)) {
        return;
    }
    int ya = imax(p.cl.y0, ifloor(by0)), yb = imin(p.cl.y1, iceil(by1));
    float inx = s.nx > -1e-7f && s.nx < 1e-7f ? 0.0f : 1.0f / s.nx;   /* reciprocals once, not per row */
    float iux = s.ux > -1e-7f && s.ux < 1e-7f ? 0.0f : 1.0f / s.ux;
    for (int y = ya; y < yb; y++) {
        float py = (float)y + 0.5f;
        ln_row_t o = { s.ny * (py - s.Y0) - s.nx * s.X0, s.uy * (py - s.Y0) - s.ux * s.X0 };
        float a0, a1, b0, b1;
        if (!lin_range(s.nx, inx, o.cs, -(s.hw + 0.5f), s.hw + 0.5f, &a0, &a1)
            || !lin_range(s.ux, iux, o.ca, -ext - 0.5f, s.L + ext + 0.5f, &b0, &b1)) {
            continue;
        }
        float lo = fmx(a0, b0), hi = fmn(a1, b1);
        if (!(lo < hi)) {
            continue;
        }
        int nz0 = imax(ifloor(lo - 0.5f), ifloor(bx0)), nz1 = imin(ifloor(hi - 0.5f) + 1, iceil(bx1));
        int f0 = 0, f1 = 0;
        if (s.hw >= 0.5f && s.L >= 1.0f
            && lin_range(s.nx, inx, o.cs, -(s.hw - 0.5f) - 1e-6f, s.hw - 0.5f + 1e-6f, &a0, &a1)
            && lin_range(s.ux, iux, o.ca, 0.5f - 1e-6f, s.L - 0.5f + 1e-6f, &b0, &b1)) {
            lo = fmx(a0, b0);
            hi = fmn(a1, b1);
            if (lo <= hi) {
                f0 = iceil(lo - 0.5f);
                f1 = ifloor(hi - 0.5f) + 1;
            }
        }
        emit_shape(&p, y, nz0, nz1, f0, f1, ln_seg, &s, &o);
    }
}

/* ---- text ---- */

/* Fonts are read with ext-display's display_text.h, so the metrics (ascent, advances, missing glyphs) are the
   ones Display::text() uses. */
typedef display_text_font_t font_t;

static int glyph_adv(const font_t *f, const GFXglyph *g) { return display_text_advance(f, g); }

typedef struct { int left, width, cut0, cut1; bool cut; } text_layout_t;

/* Text positions are kept within +-2^29 and widths within 2^30, so no sum below overflows an int. */
#define TEXT_POS_MAX (1 << 29)
static int text_pos(int v) { return v < -TEXT_POS_MAX ? -TEXT_POS_MAX : v > TEXT_POS_MAX ? TEXT_POS_MAX : v; }

static int text_width(const font_t *f, const char *s, const char *end)
{
    int64_t w = 0;
    while (s < end) {
        w += glyph_adv(f, display_text_glyph(f, display_text_utf8(&s, end)));
    }
    return w > (1 << 30) ? (1 << 30) : (int)w;
}

static text_layout_t text_layout(const font_t *f, int x, const char *s, const char *end, slate_align_t align,
                                 int max_width)
{
    text_layout_t l;
    int tw = text_width(f, s, end);
    x = text_pos(x);
    max_width = max_width > (1 << 30) ? (1 << 30) : max_width;
    l.cut = max_width > 0 && tw > max_width;
    l.width = l.cut ? max_width : tw;
    l.left = align == SLATE_ALIGN_CENTER ? x - l.width / 2 : align == SLATE_ALIGN_RIGHT ? x - l.width : x;
    l.cut0 = l.left;
    l.cut1 = l.left + l.width;
    return l;
}

slate_rect_t slate_text_bbox(int x, int y, const char *utf8, const struct display_font_entry *font,
                             slate_align_t align, int max_width)
{
    font_t f;
    if (!utf8 || !display_text_font_entry(&f, font)) {
        return (slate_rect_t){ x, y, x, y };
    }
    const char *p = utf8, *end = utf8 + strlen(utf8);
    y = text_pos(y);
    text_layout_t l = text_layout(&f, x, p, end, align, max_width);
    if (p == end) {
        return (slate_rect_t){ l.left, y, l.left, y };
    }
    slate_rect_t r = { l.left, y, l.left + l.width, y + f.line_height };
    int cx = l.left, base = y + f.ascent;
    while (p < end) {
        const GFXglyph *g = display_text_glyph(&f, display_text_utf8(&p, end));
        if (g && g->width && g->height) {
            r.x0 = imin(r.x0, cx + g->xOffset);
            r.x1 = imax(r.x1, cx + g->xOffset + g->width);
            r.y0 = imin(r.y0, base + g->yOffset);
            r.y1 = imax(r.y1, base + g->yOffset + g->height);
        }
        cx += glyph_adv(&f, g);
        if (l.cut && cx >= l.cut1 + 255) {   /* no later glyph can reach back inside the cut */
            break;
        }
    }
    if (l.cut) {
        r.x0 = imax(r.x0, l.cut0);
        r.x1 = imin(r.x1, l.cut1);
    }
    return r;
}

/* Solid rectangle [x0, x1) x [y0, y1) (screen) with the pen's colour, inside cl. */
static void text_rect(const pen_t *p, slate_rect_t cl, int x0, int y0, int x1, int y1)
{
    x0 = imax(x0, cl.x0);
    y0 = imax(y0, cl.y0);
    x1 = imin(x1, cl.x1);
    y1 = imin(y1, cl.y1);
    if (x0 < x1 && y0 < y1) {
        slate_rect_t r = { x0 - p->b->ox, y0 - p->b->oy, x1 - p->b->ox, y1 - p->b->oy };
        p->g->fill_a(p->b, r, p->c565, p->ea);
    }
}

void slate_draw_text(slate_buf_t *b, slate_rect_t clip, int x, int y, const char *utf8,
                     const struct display_font_entry *font, uint32_t argb, slate_align_t align, int max_width,
                     uint8_t a)
{
    font_t f;
    pen_t p;
    if (!utf8 || !display_text_font_entry(&f, font)
        || !pen_init(&p, b, clip, (slate_paint_t){ argb, NULL }, a, 0, 0, 0, 0)) {
        return;
    }
    const char *s = utf8, *end = utf8 + strlen(utf8);
    y = text_pos(y);
    text_layout_t l = text_layout(&f, x, s, end, align, max_width);
    slate_rect_t cl = p.cl;
    if (l.cut) {
        cl.x0 = imax(cl.x0, l.cut0);
        cl.x1 = imin(cl.x1, l.cut1);
    }
    if (cl.x0 >= cl.x1) {
        return;
    }
    unsigned max = (1u << f.bpp) - 1u, step = 255u / max;   /* level v -> coverage v * 255 / max */
    int cx = l.left, base = y + f.ascent;
    uint8_t cov[256];
    while (s < end) {
        const GFXglyph *g = display_text_glyph(&f, display_text_utf8(&s, end));
        int adv = glyph_adv(&f, g);
        if (cx - 255 >= cl.x1) {   /* every later glyph starts further right */
            break;
        }
        if (!g) {   /* missing: a box, as ext-display draws it */
            int bx0 = cx, by0 = y + 1, bx1 = cx + adv - 1, by1 = y + f.line_height - 1;
            if (bx1 > bx0 && by1 > by0) {
                text_rect(&p, cl, bx0, by0, bx1, by0 + 1);
                text_rect(&p, cl, bx0, by1 - 1, bx1, by1);
                text_rect(&p, cl, bx0, by0 + 1, bx0 + 1, by1 - 1);
                text_rect(&p, cl, bx1 - 1, by0 + 1, bx1, by1 - 1);
            }
            cx += adv;
            continue;
        }
        int gx0 = cx + g->xOffset, gy0 = base + g->yOffset;
        int i0 = imax(0, cl.x0 - gx0), i1 = imin(g->width, cl.x1 - gx0);
        int j0 = imax(0, cl.y0 - gy0), j1 = imin(g->height, cl.y1 - gy0);
        const uint8_t *bits = f.bitmap + g->bitmapOffset;
        for (int j = j0; j < j1 && i0 < i1; j++) {
            size_t bit = ((size_t)j * g->width + (size_t)i0) * (size_t)f.bpp;
            int any = 0;
            for (int i = i0; i < i1; i++, bit += (size_t)f.bpp) {
                unsigned v = (bits[bit >> 3] >> (8 - f.bpp - (bit & 7))) & max;
                cov[i - i0] = (uint8_t)(v * step);
                any |= (int)v;
            }
            if (any) {
                int xl = gx0 + i0 - b->ox, yl = gy0 + j - b->oy;
                p.g->fill_mask(b, (slate_rect_t){ xl, yl, xl + (i1 - i0), yl + 1 }, p.c565, p.ea, cov, i1 - i0);
            }
        }
        cx += adv;
    }
}
