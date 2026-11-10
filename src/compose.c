/* Band compositor of Slate: see compose.h. */
#include "slate_speed.h"   /* first: the code below is built for speed */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "compose.h"
#include "raster.h"
#include "slate_config.h"

#define OCC_MAX 8          /* occluders kept per band */
#define ONES 64            /* canvas with opacity: alpha row chunk */
#define FBIG 8388608.0f

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline float fmn(float a, float b) { return a < b ? a : b; }
static inline float fmx(float a, float b) { return a > b ? a : b; }
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
static inline int iround(float v) { return ifloor(v + 0.5f); }
static inline unsigned div255(unsigned x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

static const slate_rect_t EMPTY = { 0, 0, 0, 0 };

static inline slate_rect_t shift(slate_rect_t r, int dx, int dy)
{
    return (slate_rect_t){ r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy };
}
static inline slate_rect_t to_buf(const slate_buf_t *b, slate_rect_t r) { return shift(r, -b->ox, -b->oy); }
static inline slate_rect_t buf_rect(const slate_buf_t *b)
{
    return (slate_rect_t){ b->ox, b->oy, b->ox + b->w, b->oy + b->h };
}
static inline long area(slate_rect_t r) { return slate_rect_empty(r) ? 0 : (long)(r.x1 - r.x0) * (r.y1 - r.y0); }

/* ---- working state ---- */

typedef struct {
    slate_el_t *e;
    float pox, poy;         /* parent's screen origin */
    slate_rect_t clip;      /* ancestors' clips and the area drawn */
    slate_rect_t vis;       /* box cut by clip */
    slate_rect_t occ;       /* opaque interior cut by clip */
    bool skip;
} item_t;

static item_t *items;
static int items_cap;
static item_t *flist;   /* the frame's items over the whole screen (render() of a band filters it) */
static int flist_cap;
static uint32_t frame_no;
static int band_next;   /* the buffer the next band goes to: continuous across frames (the one freed first) */

typedef struct {
    const slate_target_t *t;
    slate_stats_t *st;
    const slate_gfx_ops_t *ops;   /* table for the buffer being drawn into */
    uint8_t *ar;
    size_t ar_size, ar_top;
    int it_n;
    uint32_t frame;
    bool und;                     /* the underlay is on for this frame */
    int fl_n;                     /* items in flist; -1: no list (out of memory) */
} cx_t;

/* The underlay (compose.h): the screen as the background colour and the bottom items that stay still (P, a prefix
   of the items in z-order) draw it, kept in cache memory, valid where `valid` says (one bit per pair of columns,
   per row). */
#define UNDER_QUIET 2   /* frames without a change before an item may join P */
static struct {
    uint16_t *px;              /* w x h, NULL: none */
    uint32_t *valid;           /* h rows of vw words */
    size_t bytes;
    int w, h, vw;
    bool on;                   /* P is defined (the flags are set) and px holds it where valid */
    bool refused;              /* gave its memory to an element cache: not taken again until the scene changes */
    int k;                     /* items in P */
    uint32_t sig, t0;          /* P's elements (hash) and the clock when P was defined */
    uint16_t bg;               /* the background colour it was drawn on */
    slate_el_t *root;          /* the root P was defined under */
    uint32_t root_self;        /* its mod_self when the underlay was refused */
    void (*free)(void *ctx, void *p);
    void *ctx;
} under;
static const slate_target_t *alloc_t;   /* the target of the frame in progress (el_alloc) */
static bool under_enabled;

void slate_compose_underlay(bool on) { under_enabled = on; }

static void under_drop(void)
{
    if (under.px && under.free) {
        under.free(under.ctx, under.px);
    }
    under.px = NULL;
    under.valid = NULL;
    under.bytes = 0;
    under.on = false;
}

void slate_compose_release(void)
{
    free(items);
    items = NULL;
    items_cap = 0;
    free(flist);
    flist = NULL;
    flist_cap = 0;
    under_drop();
    under.refused = false;
    under.root = NULL;
}

/* Memory for element caches: when it is refused while the underlay holds memory, the underlay gives it up (it is
   an optimisation, the caches are the documented behaviour) and the allocation is tried again. */
static void *el_alloc(void *ctx, size_t n)
{
    void *p = alloc_t->cache_alloc(ctx, n);
    if (!p && under.px) {
        under_drop();
        under.refused = true;
        under.root_self = under.root ? under.root->mod_self : 0;
        p = alloc_t->cache_alloc(ctx, n);
    }
    return p;
}

static void *arena_take(cx_t *cx, size_t n)
{
    n = (n + 3) & ~(size_t)3;
    if (!cx->ar || cx->ar_size - cx->ar_top < n) {
        return NULL;
    }
    void *p = cx->ar + cx->ar_top;
    cx->ar_top += n;
    return p;
}

/* Scratch on demand (compose.h, scratch_grow): with nothing in the scratch, asks the target for n bytes (at most
   scratch_max) when it has fewer. */
static void arena_need(cx_t *cx, size_t n)
{
    const slate_target_t *t = cx->t;
    n = n < t->scratch_max ? n : t->scratch_max;
    if (!t->scratch_grow || cx->ar_top != 0) {
        return;
    }
    if (n > cx->st->scratch_want) {
        cx->st->scratch_want = (uint32_t)n;
    }
    if (cx->ar && n <= cx->ar_size) {
        return;
    }
    size_t got = 0;
    void *p = t->scratch_grow(t->ctx, n, &got);
    cx->ar = p;
    cx->ar_size = p ? got : 0;
}

#if SLATE_GFX_BLUR
/* Temporary memory for building a cache (never for blending into a band): scratch when it fits, else
   cache_alloc. */
static void *tmp_alloc(cx_t *cx, size_t n, bool *arena)
{
    if (n <= cx->t->scratch_max) {
        arena_need(cx, n);
    }
    void *p = arena_take(cx, n);
    if (p) {
        *arena = true;
        return p;
    }
    *arena = false;
    return cx->t->cache_alloc ? el_alloc(cx->t->ctx, n) : NULL;
}
static void tmp_free(cx_t *cx, void *p, bool arena, size_t top)
{
    if (arena) {
        cx->ar_top = top;
    } else if (p && cx->t->cache_free) {
        cx->t->cache_free(cx->t->ctx, p);
    }
}
#endif

/* Switches drawing (raster and compositor) to ops; returns the raster's previous table for pop_ops. */
typedef struct { const slate_gfx_ops_t *ops, *raster; } ops_save_t;
static ops_save_t push_ops(cx_t *cx, const slate_gfx_ops_t *ops)
{
    ops_save_t s = { cx->ops, slate_raster_ops(ops) };
    cx->ops = ops;
    return s;
}
static void pop_ops(cx_t *cx, ops_save_t s)
{
    cx->ops = s.ops;
    slate_raster_ops(s.raster);
}

/* ---- element facts ---- */

static bool offscreen(const slate_el_t *g)
{
    return g->alpha != 255 || g->cached || slate_el_has_shadow(g) || slate_el_blur(g) > 0;
}

/* The longest chain of groups composed offscreen nested in group g (0: none); NEST_ALL when one of them is drawn
   through a cache (its image is built from the whole group, wider than the strip it sits in). */
#define NEST_ALL 30
static int nest_depth(const slate_el_t *g)
{
    int d = 0;
    for (const slate_el_t *k = g->u.g.first; k; k = k->next) {
        if (k->kind != SLATE_EL_GROUP) {
            continue;
        }
        if (k->cached || slate_el_has_shadow(k) || slate_el_blur(k) > 0) {
            return NEST_ALL;
        }
        int kd = nest_depth(k) + (k->alpha != 255);
        if (kd >= NEST_ALL) {
            return NEST_ALL;
        }
        d = kd > d ? kd : d;
    }
    return d;
}

/* e's own origin on screen (pox, poy: its parent's origin, placed or not). */
static void own_origin(const slate_el_t *e, float pox, float poy, float *x, float *y)
{
    slate_el_place(e, &pox, &poy);
    *x = pox + e->x;
    *y = poy + e->y;
}

static slate_rect_t opaque_rect(const slate_el_t *e, float pox, float poy)
{
    if (!e->opaque) {
        return EMPTY;
    }
    float x, y;
    own_origin(e, pox, poy, &x, &y);
    const slate_shape_t *s = &e->u.shape;
    bool thin = s->border > 0 && (s->border_argb >> 24) != 255;   /* a border that lets things through */
    slate_rect_t r = EMPTY;
    switch (e->kind) {
    case SLATE_EL_ROUNDRECT: {
        if (!(s->w > 0 && s->h > 0)) {
            return EMPTY;
        }
        r = (slate_rect_t){ iceil(x), iceil(y), ifloor(x + s->w), ifloor(y + s->h) };
        if (thin) {
            int b = iceil(s->border);
            r = (slate_rect_t){ r.x0 + b, r.y0 + b, r.x1 - b, r.y1 - b };
        }
        float rad = fmn(s->r, fmn(s->w, s->h) * 0.5f);
        int k = rad > 0 ? iceil(rad) : 0;
        if (k > 0) {
            slate_rect_t a = { r.x0 + k, r.y0, r.x1 - k, r.y1 }, b = { r.x0, r.y0 + k, r.x1, r.y1 - k };
            r = area(a) >= area(b) ? a : b;
        }
        break;
    }
    case SLATE_EL_CIRCLE: {
        float rr = s->r - (thin ? s->border : 0.0f) - 0.5f;
        if (!(rr > 0)) {
            return EMPTY;
        }
        float h = rr * 0.70710678f;
        r = (slate_rect_t){ iceil(x - h), iceil(y - h), ifloor(x + h), ifloor(y + h) };
        break;
    }
    case SLATE_EL_CANVAS: {
        int X = iround(x), Y = iround(y);
        r = (slate_rect_t){ X, Y, X + e->u.canvas.w, Y + e->u.canvas.h };
        break;
    }
    default:
        return EMPTY;
    }
    return slate_rect_empty(r) ? EMPTY : r;
}

/* ---- drawing ---- */

static void render(cx_t *cx, slate_buf_t *buf, slate_el_t *e, float pox, float poy, slate_rect_t clip, bool children,
                   bool bg);

/* An A8 mask covering screen rect r (stride = r's width) drawn in colour argb times a, cut by clip. */
static void draw_mask(cx_t *cx, slate_buf_t *buf, slate_rect_t r, const uint8_t *m, uint32_t argb, unsigned a,
                      slate_rect_t clip)
{
    slate_rect_t v = slate_rect_and(slate_rect_and(r, clip), buf_rect(buf));
    unsigned ea = div255((argb >> 24) * a);
    if (slate_rect_empty(v) || ea == 0) {
        return;
    }
    int stride = r.x1 - r.x0;
    const uint8_t *mm = m + (size_t)(v.y0 - r.y0) * stride + (v.x0 - r.x0);
    cx->ops->fill_mask(buf, to_buf(buf, v), slate_rgb565(argb & 0xFFFFFF), (uint8_t)ea, mm, stride);
}

static void draw_canvas(cx_t *cx, slate_buf_t *buf, const slate_el_t *e, float x, float y, slate_rect_t clip,
                        uint8_t a)
{
    const slate_canvas_t *c = &e->u.canvas;
    if (!c->px) {
        return;
    }
    int X = iround(x) - buf->ox, Y = iround(y) - buf->oy;
    slate_rect_t cl = to_buf(buf, clip);
    if (a == 255) {
        cx->ops->copy(buf, X, Y, c->px, c->w, c->h, c->w, cl);
        return;
    }
    uint8_t ones[ONES];
    memset(ones, 255, sizeof ones);
    for (int j = imax(0, cl.y0 - Y); j < c->h && Y + j < cl.y1; j++) {
        for (int i = 0; i < c->w; i += ONES) {
            int n = imin(ONES, c->w - i);
            cx->ops->copy_a(buf, X + i, Y + j, c->px + (size_t)j * c->w + i, ones, n, 1, n, a, cl);
        }
    }
}

/* The shape alone (no shadow) in its colours; white = coverage only. */
static void draw_shape(slate_buf_t *buf, const slate_el_t *e, float pox, float poy, slate_rect_t clip, uint8_t a,
                       bool white)
{
    float x, y;
    own_origin(e, pox, poy, &x, &y);
    const slate_paint_t W = { 0xFFFFFFFFu, NULL };
    switch (e->kind) {
    case SLATE_EL_ROUNDRECT: {
        const slate_shape_t *s = &e->u.shape;
        slate_draw_roundrect(buf, clip, x, y, s->w, s->h, s->r, white ? W : s->fill, s->border,
                             white ? 0xFFFFFFFFu : s->border_argb, a);
        break;
    }
    case SLATE_EL_CIRCLE: {
        const slate_shape_t *s = &e->u.shape;
        slate_draw_circle(buf, clip, x, y, s->r, white ? W : s->fill, s->border, white ? 0xFFFFFFFFu : s->border_argb,
                          a);
        break;
    }
    case SLATE_EL_ARC: {
        const slate_arc_t *s = &e->u.arc;
        slate_draw_arc(buf, clip, x, y, s->r, s->from, s->to, s->width, (slate_caps_t)s->caps, s->color, a);
        break;
    }
    case SLATE_EL_LINE: {
        const slate_line_t *s = &e->u.line;
        slate_draw_line(buf, clip, x, y, x - e->x + s->x1, y - e->y + s->y1, s->width, (slate_caps_t)s->caps, s->argb,
                        a);
        break;
    }
    case SLATE_EL_TEXT: {
        const slate_text_t *s = &e->u.text;
        slate_draw_text(buf, clip, iround(x), iround(y), s->text, s->font, s->argb, (slate_align_t)s->align,
                        s->max_width, a);
        break;
    }
    default:
        break;
    }
}

/* ---- offscreen strips ---- */

typedef void (*sink_fn)(cx_t *cx, const slate_bufa_t *strip, void *arg);

/* Composes the children of group g (parent origin pox, poy) over the screen rect R, cut by cc, into transparent
   RGB565 + A8 strips from scratch (half of what is left) and hands each to sink. false (nothing done) when
   scratch cannot hold one row: offscreen work never blends in slow memory. */
static bool produce(cx_t *cx, slate_el_t *g, float pox, float poy, slate_rect_t R, slate_rect_t cc, sink_fn sink,
                    void *arg)
{
    int w = R.x1 - R.x0, h = R.y1 - R.y0;
    if (w <= 0 || h <= 0) {
        return true;
    }
    size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;
    size_t top = cx->ar_top;
    if (top == 0 && cx->t->scratch_grow) {
        /* the strips scratch_max gives, and room for the groups nested in g (each level gets half of what is
           left; theirs are no wider than g's strip) */
        size_t max = cx->t->scratch_max, kmax = max / 2 / row, want = 2 * row * (kmax < (size_t)h ? kmax : (size_t)h);
        if (want < max) {
            int d = nest_depth(g);
            size_t deep = 2 * (row + 8);   /* 2^(d+1) rows, with room for the rounding */
            for (int i = 0; i < d && deep < max; i++) {
                deep *= 2;
            }
            want = deep > want ? deep : want;
        }
        arena_need(cx, want);
    }
    size_t avail = cx->ar ? (cx->ar_size - cx->ar_top) / 2 : 0;
    int k = (int)(avail / row < (size_t)h ? avail / row : (size_t)h);
    if (k < 1) {
        return false;
    }
    size_t pxb = ((size_t)k * w * 2 + 3) & ~(size_t)3;
    uint8_t *mem = arena_take(cx, pxb + (size_t)k * w);
    if (!mem) {
        return false;
    }
    uint16_t *px = (uint16_t *)mem;
    uint8_t *a = mem + pxb;
    for (int y = R.y0; y < R.y1; y += k) {
        int n = imin(k, R.y1 - y);
        memset(px, 0, (size_t)n * w * 2);
        memset(a, 0, (size_t)n * w);
        slate_bufa_t off = { { px, w, n, w, R.x0, y }, a };
        ops_save_t s = push_ops(cx, slate_a8_ops());
        render(cx, &off.b, g, pox, poy, slate_rect_and(cc, buf_rect(&off.b)), true, false);
        pop_ops(cx, s);
        sink(cx, &off, arg);
    }
    cx->ar_top = top;
    return true;
}

typedef struct { slate_buf_t *buf; slate_rect_t clip; uint8_t alpha; } band_sink_t;

static void sink_band(cx_t *cx, const slate_bufa_t *s, void *arg)
{
    band_sink_t *bs = arg;
    cx->ops->copy_a(bs->buf, s->b.ox - bs->buf->ox, s->b.oy - bs->buf->oy, s->b.px, s->a, s->b.w, s->b.h, s->b.stride,
                    bs->alpha, to_buf(bs->buf, bs->clip));
}

static void sink_cache(cx_t *cx, const slate_bufa_t *s, void *arg)
{
    (void)cx;
    slate_bufa_t *d = arg;
    int dy = s->b.oy - d->b.oy, dx = s->b.ox - d->b.ox;
    for (int j = 0; j < s->b.h; j++) {
        size_t o = (size_t)(j + dy) * d->b.stride + dx;
        memcpy(d->b.px + o, s->b.px + (size_t)j * s->b.stride, (size_t)s->b.w * 2);
        memcpy(d->a + o, s->a + (size_t)j * s->b.stride, (size_t)s->b.w);
    }
}

/* ---- caches ---- */

#if SLATE_HAS_SHADOWS
/* Builds c's shadow mask from the alpha `src` (the content image, w x h at rel; NULL: e's shape, drawn here into
   the screen rect need). */
static void build_shadow(cx_t *cx, slate_el_t *e, slate_cache_t *c, const uint8_t *src, float pox, float poy,
                         slate_rect_t need, slate_rect_t rel)
{
    const slate_target_t *t = cx->t;
    const slate_shadow_t *sd = slate_el_shadow(e);
    int w = need.x1 - need.x0, h = need.y1 - need.y0;
    int sr = sd->blur, g = SLATE_BLUR_EXTENT(sr);
    int sw = w + 2 * g, sh = h + 2 * g;
    uint8_t *m = slate_cache_mem(c, (void **)&c->sa, &c->sbytes, (size_t)sw * sh, el_alloc, t->cache_free, t->ctx);
    if (!m) {
        return;
    }
    memset(m, 0, (size_t)sw * sh);
    size_t top = cx->ar_top;
    bool arena;
    if (src) {
        for (int j = 0; j < h; j++) {
            memcpy(m + (size_t)(j + g) * sw + g, src + (size_t)j * w, (size_t)w);
        }
    } else {
        /* the shape's coverage, drawn in strips of rows that fit in scratch (internal RAM; a shape drawn strip by
           strip gives the same pixels as in one piece, raster.h), else in one piece from tmp_alloc */
        size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;
        arena_need(cx, row * (size_t)h);
        size_t avail = cx->ar ? cx->ar_size - cx->ar_top : 0;
        int k = (int)(avail / row < (size_t)h ? avail / row : (size_t)h);
        if (k < 1) {
            k = h;
        }
        size_t pxb = ((size_t)w * k * 2 + 3) & ~(size_t)3;
        uint8_t *tmp = tmp_alloc(cx, pxb + (size_t)w * k, &arena);
        if (!tmp) {
            return;
        }
        for (int y = 0; y < h; y += k) {
            int n = imin(k, h - y);
            memset(tmp, 0, pxb + (size_t)w * k);
            slate_bufa_t off = { { (uint16_t *)tmp, w, n, w, need.x0, need.y0 + y }, tmp + pxb };
            slate_rect_t strip = { need.x0, need.y0 + y, need.x1, need.y0 + y + n };
            ops_save_t s = push_ops(cx, slate_a8_ops());
            draw_shape(&off.b, e, pox, poy, strip, 255, true);
            pop_ops(cx, s);
            for (int j = 0; j < n; j++) {
                memcpy(m + (size_t)(y + j + g) * sw + g, off.a + (size_t)j * w, (size_t)w);
            }
        }
        tmp_free(cx, tmp, arena, top);
    }
    if (sr > 0) {
        void *bt = tmp_alloc(cx, SLATE_BLUR_TMP_A(sw, sh), &arena);
        if (!bt) {
            return;   /* not marked built: retried next frame */
        }
        slate_gfx()->blur(NULL, m, sw, sh, sr, bt);
        tmp_free(cx, bt, arena, top);
    }
    c->srel = (slate_rect_t){ rel.x0 - g + sd->dx, rel.y0 - g + sd->dy, rel.x1 + g + sd->dx, rel.y1 + g + sd->dy };
    c->shadow_ok = true;
}
#endif

/* e's cache, ready for this frame, or NULL (not usable: no memory, nothing to draw). Builds what is stale.
   pox, poy: e's parent origin. */
static slate_cache_t *resolve(cx_t *cx, slate_el_t *e, float pox, float poy)
{
    const slate_target_t *t = cx->t;
    if (!t->cache_alloc) {
        return NULL;
    }
    slate_cache_t *c = slate_cache_of(e);
    if (!c) {
        return NULL;
    }
    if (c->frame == cx->frame) {
        return c->usable ? c : NULL;
    }
    c->frame = cx->frame;
    c->usable = false;
    slate_el_place(e, &pox, &poy);
    int ix = iround(pox + e->x), iy = iround(poy + e->y);   /* whole pixels: e snaps */
    bool group = e->kind == SLATE_EL_GROUP, shadow = slate_el_has_shadow(e);
    const slate_shadow_t *sd = slate_el_shadow(e);
    int reach = SLATE_BLUR_EXTENT(slate_el_blur(e)) +
                (shadow ? SLATE_BLUR_EXTENT(sd->blur) + abs(sd->dx) + abs(sd->dy) : 0);
    slate_rect_t scr = { -reach, -reach, t->w + reach, t->h + reach };
    slate_rect_t need = slate_rect_and(slate_el_screen_ext(e, pox, poy, true), scr);
    if (slate_rect_empty(need)) {
        return NULL;
    }
    slate_rect_t rel = shift(need, -ix, -iy);
    bool placed = slate_rect_contains(c->crel, rel);
    bool content = placed && (!group || c->content_ok), shade = placed && (!shadow || c->shadow_ok);
    if (content && shade) {
        cx->st->cache_hit++;
        c->usable = true;
        return c;
    }
    cx->st->cache_miss++;
    if (content) {   /* only the shadow is stale: rebuilt from the image that is there */
        rel = c->crel;
        need = shift(rel, ix, iy);
    }
    c->shadow_ok = false;
    c->crel = rel;
    bool keep = true;   /* false: usable this frame only (a blur could not be made), rebuilt next frame */
    if (group && !content) {
        c->content_ok = false;
        int w = need.x1 - need.x0, h = need.y1 - need.y0;
        size_t pxb = ((size_t)w * h * 2 + 3) & ~(size_t)3;
        if (!slate_cache_mem(c, (void **)&c->px, &c->cbytes, pxb + (size_t)w * h, el_alloc, t->cache_free, t->ctx)) {
            c->crel = EMPTY;
            return NULL;
        }
        c->a = (uint8_t *)c->px + pxb;
        memset(c->px, 0, pxb + (size_t)w * h);
        slate_bufa_t dst = { { c->px, w, h, w, need.x0, need.y0 }, c->a };
        slate_rect_t cc = need;
        if (slate_el_clips(e)) {
            cc = slate_rect_and(cc, slate_el_clip_rect(e, pox, poy));
        }
        if (!produce(cx, e, pox, poy, need, cc, sink_cache, &dst)) {
            c->crel = EMPTY;   /* no scratch: no build (and no blending in cache memory) */
            return NULL;
        }
#if SLATE_HAS_BLUR
        if (slate_el_blur(e) > 0) {
            size_t top = cx->ar_top;
            bool arena;
            void *bt = tmp_alloc(cx, SLATE_BLUR_TMP(w, h), &arena);
            if (bt) {
                slate_gfx()->blur(c->px, c->a, w, h, slate_el_blur(e), bt);
                tmp_free(cx, bt, arena, top);
            } else {
                keep = false;
            }
        }
#endif
        c->content_ok = keep;
    }
#if SLATE_HAS_SHADOWS
    if (shadow) {
        build_shadow(cx, e, c, group ? c->a : NULL, pox, poy, need, rel);
    }
#endif
    c->usable = group ? true : c->shadow_ok;
    return c->usable ? c : NULL;
}

/* ---- items ---- */

static void draw_shadow_of(cx_t *cx, slate_buf_t *buf, slate_el_t *e, const slate_cache_t *c, float pox, float poy,
                           slate_rect_t clip, uint8_t a)
{
    if (!c || !c->shadow_ok || !c->sa) {
        return;
    }
    float x, y;
    own_origin(e, pox, poy, &x, &y);
    draw_mask(cx, buf, shift(c->srel, iround(x), iround(y)), c->sa, slate_el_shadow(e)->argb, a, clip);
}

static void draw_group(cx_t *cx, slate_buf_t *buf, const item_t *it)
{
    slate_el_t *e = it->e;
    slate_rect_t cc = it->clip;
    if (slate_el_clips(e)) {
        cc = slate_rect_and(cc, slate_el_clip_rect(e, it->pox, it->poy));
    }
    if (e->cached || slate_el_has_shadow(e) || slate_el_blur(e) > 0) {
        slate_cache_t *c = resolve(cx, e, it->pox, it->poy);
        if (c) {
            float x, y;
            own_origin(e, it->pox, it->poy, &x, &y);
            if (slate_el_has_shadow(e)) {
                draw_shadow_of(cx, buf, e, c, it->pox, it->poy, it->clip, e->alpha);
            }
            slate_rect_t r = shift(c->crel, iround(x), iround(y));
            int w = r.x1 - r.x0;
            cx->ops->copy_a(buf, r.x0 - buf->ox, r.y0 - buf->oy, c->px, c->a, w, r.y1 - r.y0, w, e->alpha,
                            to_buf(buf, it->clip));
            return;
        }
    }
    if (e->alpha == 255) {
        render(cx, buf, e, it->pox, it->poy, cc, true, false);
        return;
    }
    slate_rect_t R = slate_rect_and(slate_el_screen_ext(e, it->pox, it->poy, true), cc);
    band_sink_t bs = { buf, it->clip, e->alpha };
    if (!produce(cx, e, it->pox, it->poy, R, cc, sink_band, &bs)) {
        cx->st->degraded++;   /* no scratch: drawn without the group opacity */
        render(cx, buf, e, it->pox, it->poy, cc, true, false);
    }
}

static void draw_item(cx_t *cx, slate_buf_t *buf, const item_t *it)
{
    slate_el_t *e = it->e;
    cx->st->drawn++;
    if (e->kind == SLATE_EL_GROUP) {
        draw_group(cx, buf, it);
        return;
    }
    if (slate_el_has_shadow(e)) {
        /* the shape is drawn over its shadow and writes every pixel of its opaque interior (it->occ) whatever is
           under it (the occluder rule): the shadow is drawn around that interior only */
        const slate_cache_t *c = resolve(cx, e, it->pox, it->poy);
        slate_rect_t cl = it->clip, o = slate_rect_and(it->occ, cl);
        if (slate_rect_empty(o)) {
            draw_shadow_of(cx, buf, e, c, it->pox, it->poy, cl, e->alpha);
        } else {
            slate_rect_t part[4] = {
                { cl.x0, cl.y0, cl.x1, o.y0 }, { cl.x0, o.y1, cl.x1, cl.y1 },   /* above, below */
                { cl.x0, o.y0, o.x0, o.y1 }, { o.x1, o.y0, cl.x1, o.y1 },       /* left, right */
            };
            for (int k = 0; k < 4; k++) {
                if (!slate_rect_empty(part[k])) {
                    draw_shadow_of(cx, buf, e, c, it->pox, it->poy, part[k], e->alpha);
                }
            }
        }
    }
    if (e->kind == SLATE_EL_CANVAS) {
        draw_canvas(cx, buf, e, it->pox + e->x, it->poy + e->y, it->clip, e->alpha);
    } else {
        draw_shape(buf, e, it->pox, it->poy, it->clip, e->alpha, false);
    }
}

static bool items_room(cx_t *cx)
{
    if (cx->it_n < items_cap) {
        return true;
    }
    int cap = items_cap ? items_cap * 2 : 64;
    item_t *n = realloc(items, (size_t)cap * sizeof *n);
    if (!n) {
        return false;
    }
    items = n;
    items_cap = cap;
    return true;
}

/* Lists e (flattening plain groups) as items, or draws them at once into direct. false: out of memory. */
static bool collect(cx_t *cx, slate_el_t *e, float pox, float poy, slate_rect_t clip, slate_buf_t *direct)
{
    if (!e->visible || e->alpha == 0) {
        return true;
    }
    slate_el_place(e, &pox, &poy);
    slate_rect_t vis = slate_rect_and(slate_el_screen_ext(e, pox, poy, false), clip);
    if (slate_rect_empty(vis)) {
        return true;
    }
    if (e->kind == SLATE_EL_GROUP && !offscreen(e)) {
        slate_rect_t cc = clip;
        if (slate_el_clips(e)) {
            cc = slate_rect_and(cc, slate_el_clip_rect(e, pox, poy));
        }
        float gx = pox + e->x, gy = poy + e->y;
        for (slate_el_t *k = e->u.g.first; k; k = k->next) {
            if (!collect(cx, k, gx, gy, cc, direct)) {
                return false;
            }
        }
        return true;
    }
    item_t it = { e, pox, poy, clip, vis, slate_rect_and(opaque_rect(e, pox, poy), clip), false };
    if (slate_rect_empty(it.occ)) {
        it.occ = EMPTY;
    }
    if (direct) {
        draw_item(cx, direct, &it);
        return true;
    }
    if (!items_room(cx)) {
        return false;
    }
    items[cx->it_n++] = it;
    return true;
}

static void add_occluder(slate_rect_t *occ, int *no, slate_rect_t r)
{
    if (*no < OCC_MAX) {
        occ[(*no)++] = r;
        return;
    }
    int k = 0;
    for (int i = 1; i < OCC_MAX; i++) {
        if (area(occ[i]) < area(occ[k])) {
            k = i;
        }
    }
    if (area(r) > area(occ[k])) {
        occ[k] = r;
    }
}

/* r lies in the hole of arc e (whose parent origin is pox, poy): the arc writes no pixel there. Its pixels, caps
   included, are at least r - width / 2 from the centre, and a pixel whose centre is half a pixel inside that gets
   no coverage; a pixel more of margin. */
static bool in_arc_hole(const slate_el_t *e, float pox, float poy, slate_rect_t r)
{
    const slate_arc_t *a = &e->u.arc;
    float ri = a->r - a->width * 0.5f - 1.0f;
    if (!(ri > 0.0f)) {
        return false;
    }
    float cx, cy;
    own_origin(e, pox, poy, &cx, &cy);
    float dx = fmx(fabsf((float)r.x0 - cx), fabsf((float)r.x1 - cx));
    float dy = fmx(fabsf((float)r.y0 - cy), fabsf((float)r.y1 - cy));
    return dx * dx + dy * dy < ri * ri;
}

/* The items of the band clip: the frame's list cut to it (the same items, boxes and clips collect() gives for
   that clip, without walking the tree again). */
static bool band_list(cx_t *cx, slate_rect_t clip)
{
    for (int i = 0; i < cx->fl_n; i++) {
        const item_t *f = &flist[i];
        slate_rect_t vis = slate_rect_and(f->vis, clip);
        if (slate_rect_empty(vis) || (f->e->kind == SLATE_EL_ARC && in_arc_hole(f->e, f->pox, f->poy, vis))) {
            continue;
        }
        if (!items_room(cx)) {
            return false;
        }
        item_t it = *f;
        it.clip = slate_rect_and(f->clip, clip);
        it.vis = vis;
        it.occ = slate_rect_and(f->occ, clip);
        if (slate_rect_empty(it.occ)) {
            it.occ = EMPTY;
        }
        items[cx->it_n++] = it;
    }
    return true;
}

/* The frame's list: the items of the whole screen, in z-order. */
static void frame_list(cx_t *cx, slate_el_t *root, float pox, float poy)
{
    cx->fl_n = -1;
    cx->it_n = 0;
    if (!collect(cx, root, pox, poy, (slate_rect_t){ 0, 0, cx->t->w, cx->t->h }, NULL)) {
        return;
    }
    if (cx->it_n > flist_cap) {
        item_t *n = realloc(flist, (size_t)cx->it_n * sizeof *n);
        if (!n) {
            return;
        }
        flist = n;
        flist_cap = cx->it_n;
    }
    if (cx->it_n > 0) {
        memcpy(flist, items, (size_t)cx->it_n * sizeof *flist);
    }
    cx->fl_n = cx->it_n;
    cx->it_n = 0;
}

/* Occlusion over items [from, from + n): marks the hidden ones skipped; true when clip lies inside an occluder. */
static bool cull_big(cx_t *cx, int from, int n, slate_rect_t clip, slate_rect_t *big);

static bool cull(cx_t *cx, int from, int n, slate_rect_t clip)
{
    return cull_big(cx, from, n, clip, NULL);
}

/* cull, and *big (when not NULL): the occluder that covers the most of clip (EMPTY: none). */
static bool cull_big(cx_t *cx, int from, int n, slate_rect_t clip, slate_rect_t *big)
{
    slate_rect_t occ[OCC_MAX];
    int no = 0;
    for (int i = n - 1; i >= 0; i--) {
        item_t *it = &items[from + i];
        for (int k = 0; k < no; k++) {
            if (slate_rect_contains(occ[k], it->vis)) {
                it->skip = true;
                cx->st->culled++;
                break;
            }
        }
        if (!it->skip && !slate_rect_empty(it->occ)) {
            add_occluder(occ, &no, it->occ);
        }
    }
    long best = 0;
    if (big) {
        *big = EMPTY;
    }
    for (int k = 0; k < no; k++) {
        if (slate_rect_contains(occ[k], clip)) {
            return true;
        }
        long a = area(slate_rect_and(occ[k], clip));
        if (big && a > best) {
            best = a;
            *big = slate_rect_and(occ[k], clip);
        }
    }
    return false;
}

static void draw_items(cx_t *cx, slate_buf_t *buf, int from, int n)
{
    for (int i = 0; i < n; i++) {
        item_t it = items[from + i];   /* a copy: nested lists may move the array */
        if (!it.skip) {
            draw_item(cx, buf, &it);
        }
    }
}

/* ---- underlay ---- */

/* Columns [x0, x1) of rows [y0, y1) as bits of `valid` (one per pair of columns). */
static bool under_valid(slate_rect_t r)
{
    int b0 = r.x0 >> 1, b1 = (r.x1 + 1) >> 1;
    for (int y = r.y0; y < r.y1; y++) {
        const uint32_t *row = under.valid + (size_t)y * under.vw;
        for (int b = b0; b < b1;) {
            int w = b >> 5, o = b & 31, n = imin(32 - o, b1 - b);
            uint32_t m = (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u)) << o;
            if ((row[w] & m) != m) {
                return false;
            }
            b += n;
        }
    }
    return true;
}

static void under_mark(slate_rect_t r)
{
    int b0 = r.x0 >> 1, b1 = (r.x1 + 1) >> 1;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *row = under.valid + (size_t)y * under.vw;
        for (int b = b0; b < b1;) {
            int w = b >> 5, o = b & 31, n = imin(32 - o, b1 - b);
            row[w] |= (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u)) << o;
            b += n;
        }
    }
}

/* Items that may stay in the underlay: their pixels depend only on the scene (no cache or scratch behind them). */
static bool under_ok(const slate_el_t *e)
{
    return !(e->kind == SLATE_EL_GROUP && offscreen(e)) && !slate_el_has_shadow(e);
}

/* Items that are cheap to draw again: a solid fill, a canvas copy. P made only of them is not worth an underlay. */
static bool under_cheap(const slate_el_t *e)
{
    switch (e->kind) {
    case SLATE_EL_ROUNDRECT: case SLATE_EL_CIRCLE:
        return !e->u.shape.fill.grad && !(e->u.shape.border > 0);
    case SLATE_EL_CANVAS:
        return true;
    default:
        return false;
    }
}

typedef struct {
    uint32_t mark;     /* stamps must be before this (validate) ... */
    bool quiet;        /* ... or UNDER_QUIET frames old (define) */
    bool set;          /* set the flags of the items taken */
    int k, limit;
    uint32_t sig;
    bool worth;
} uwalk_t;

static bool stamp_ok(const uwalk_t *w, uint32_t stamp)
{
    uint32_t now = slate_scene_clock();
    return w->quiet ? now - stamp > UNDER_QUIET : (int32_t)(stamp - w->mark) < 0;
}

/* Walks the items under e in z-order (plain groups flattened, as collect() does) while they qualify; false once one
   does not (the prefix ends there). */
static bool under_walk(uwalk_t *w, slate_el_t *e)
{
    if (!stamp_ok(w, e->mod_self)) {
        return false;
    }
    if (e->kind == SLATE_EL_GROUP && !offscreen(e)) {
        for (slate_el_t *k = e->u.g.first; k; k = k->next) {
            if (!under_walk(w, k)) {
                return false;
            }
        }
        return true;
    }
    if (w->k == w->limit || !stamp_ok(w, e->mod_sub) || !under_ok(e)) {
        return false;
    }
    w->k++;
    w->sig = (w->sig ^ (uint32_t)(uintptr_t)e) * 16777619u;
    w->worth = w->worth || (e->visible && e->alpha != 0 && !under_cheap(e));
    if (w->set) {
        e->under = true;
    }
    return true;
}

static void under_clear_flags(slate_el_t *e)
{
    e->under = false;
    if (e->kind == SLATE_EL_GROUP) {
        for (slate_el_t *k = e->u.g.first; k; k = k->next) {
            under_clear_flags(k);
        }
    }
}

/* At the start of a frame: keeps the underlay while P has not changed, else drops its content and defines P again
   from the items that have been still for a while. cx->und: the underlay is on for this frame. */
static void under_frame(cx_t *cx, slate_el_t *root)
{
    const slate_target_t *t = cx->t;
    cx->und = false;
    if (!under_enabled || !t->cache_alloc || !t->cache_free) {
        return;
    }
    if (under.refused && (root != under.root || root->mod_self != under.root_self)) {
        under.refused = false;   /* another scene: the underlay may take memory again */
    }
    if (under.on) {
        uwalk_t w = { under.t0, false, false, 0, under.k, 2166136261u, false };
        under_walk(&w, root);
        if (w.k == under.k && w.sig == under.sig && under.px && under.w == t->w && under.h == t->h &&
            under.bg == t->background && root == under.root) {
            cx->und = true;
            return;
        }
        under.on = false;
    }
    if (under.refused) {
        return;
    }
    uwalk_t w = { 0, true, false, 0, 1 << 30, 2166136261u, false };
    under_walk(&w, root);
    if (w.k == 0 || !w.worth) {
        return;
    }
    int vw = ((t->w + 1) / 2 + 31) / 32;
    size_t pxb = ((size_t)t->w * t->h * 2 + 3) & ~(size_t)3, need = pxb + (size_t)t->h * vw * 4;
    if (!under.px || under.bytes < need || under.free != t->cache_free || under.ctx != t->ctx) {
        under_drop();
        under.px = t->cache_alloc(t->ctx, need);
        if (!under.px) {
            return;
        }
        under.bytes = need;
        under.free = t->cache_free;
        under.ctx = t->ctx;
    }
    under.w = t->w;
    under.h = t->h;
    under.vw = vw;
    under.valid = (uint32_t *)((uint8_t *)under.px + pxb);
    memset(under.valid, 0, (size_t)t->h * vw * 4);
    under_clear_flags(root);
    w = (uwalk_t){ 0, true, true, 0, 1 << 30, 2166136261u, false };
    under_walk(&w, root);
    under.k = w.k;
    under.sig = w.sig;
    under.t0 = slate_scene_clock();
    under.root = root;
    under.bg = t->background;
    under.on = true;
    cx->und = true;
}

/* Draws e (children: e's children instead) into buf within clip; bg: fill the background first. pox, poy: e's
   parent origin. */
static void render(cx_t *cx, slate_buf_t *buf, slate_el_t *e, float pox, float poy, slate_rect_t clip, bool children,
                   bool bg)
{
    clip = slate_rect_and(clip, buf_rect(buf));
    if (slate_rect_empty(clip)) {
        return;
    }
    float gx = 0, gy = 0;
    if (children) {
        own_origin(e, pox, poy, &gx, &gy);
    }
    int base = cx->it_n;
    bool ok = true;
    if (bg && cx->fl_n >= 0) {
        ok = band_list(cx, clip);
    } else if (children) {
        for (slate_el_t *k = e->u.g.first; k && ok; k = k->next) {
            ok = collect(cx, k, gx, gy, clip, NULL);
        }
    } else {
        ok = collect(cx, e, pox, poy, clip, NULL);
    }
    if (!ok) {   /* no memory for the list: draw everything in order, without occlusion */
        cx->it_n = base;
        if (bg) {
            cx->ops->fill(buf, to_buf(buf, clip), cx->t->background);
        }
        if (children) {
            for (slate_el_t *k = e->u.g.first; k; k = k->next) {
                collect(cx, k, gx, gy, clip, buf);
            }
        } else {
            collect(cx, e, pox, poy, clip, buf);
        }
        return;
    }
    int n = cx->it_n - base, np = 0;
    if (bg && cx->und && under.on) {   /* the underlay's items come first (on: it was not given up meanwhile) */
        while (np < n && items[base + np].e->under) {
            np++;
        }
    }
    if (np > 0 && under_valid(clip)) {
        /* the underlay holds the background and P here: copied instead of drawn, except under the largest
           occluder (an item above writes every pixel there whatever is under it) */
        slate_rect_t o;
        bool covered = cull_big(cx, base + np, n - np, clip, &o);
        if (!covered) {
            for (int y = clip.y0; y < clip.y1; y++) {
                uint16_t *dst = buf->px + (size_t)(y - buf->oy) * buf->stride - buf->ox;
                const uint16_t *src = under.px + (size_t)y * under.w;
                if (y >= o.y0 && y < o.y1) {
                    memcpy(dst + clip.x0, src + clip.x0, (size_t)(o.x0 - clip.x0) * 2);
                    memcpy(dst + o.x1, src + o.x1, (size_t)(clip.x1 - o.x1) * 2);
                } else {
                    memcpy(dst + clip.x0, src + clip.x0, (size_t)(clip.x1 - clip.x0) * 2);
                }
            }
        }
        draw_items(cx, buf, base + np, n - np);
    } else if (np > 0) {
        /* the background and P drawn alone (nothing above them hides them), kept in the underlay, then the rest */
        if (!cull(cx, base, np, clip)) {
            cx->ops->fill(buf, to_buf(buf, clip), cx->t->background);
        }
        draw_items(cx, buf, base, np);
        if (under.on) {
            for (int y = clip.y0; y < clip.y1; y++) {
                memcpy(under.px + (size_t)y * under.w + clip.x0,
                       buf->px + (size_t)(y - buf->oy) * buf->stride + (clip.x0 - buf->ox),
                       (size_t)(clip.x1 - clip.x0) * 2);
            }
            under_mark(clip);
        }
        cull(cx, base + np, n - np, clip);
        draw_items(cx, buf, base + np, n - np);
    } else {
        bool covered = cull(cx, base, n, clip);
        if (bg && !covered) {
            cx->ops->fill(buf, to_buf(buf, clip), cx->t->background);
        }
        draw_items(cx, buf, base, n);
    }
    cx->it_n = base;
}

/* ---- frame ---- */

void slate_compose(slate_el_t *root, slate_dirty_t *d, const slate_target_t *t, slate_emit_fn emit, void *ctx,
                   slate_stats_t *st)
{
    slate_stats_t dummy;
    if (!st) {
        st = &dummy;
    }
    memset(st, 0, sizeof *st);
    if (!d || d->n == 0 || !root || !t || !t->band_buf || t->rows <= 0 || !emit) {
        return;
    }
    uint16_t *bufs[2] = { t->band_buf(t->ctx, 0), t->band_buf(t->ctx, 1) };
    if (!bufs[0] || !bufs[1]) {
        st->aborted = 1;   /* nothing emitted, d kept: the engine can retry */
        return;
    }
    if (++frame_no == 0) {
        frame_no = 1;
    }
    cx_t cx = { t, st, slate_gfx(), t->scratch, t->scratch ? t->scratch_bytes : 0, 0, 0, frame_no, false, -1 };
    alloc_t = t;
    slate_scene_tick();
    under_frame(&cx, root);

    /* regions top to bottom */
    static slate_rect_t reg[SLATE_DIRTY_MAX];   /* not on the stack: the compositor's stack is small */
    int nr = 0;
    for (int i = 0; i < d->n && i < SLATE_DIRTY_MAX; i++) {
        slate_rect_t r = slate_rect_and(d->r[i], (slate_rect_t){ 0, 0, t->w, t->h });
        if (slate_rect_empty(r)) {
            continue;
        }
        int j = nr++;
        for (; j > 0 && (reg[j - 1].y0 > r.y0 || (reg[j - 1].y0 == r.y0 && reg[j - 1].x0 > r.x0)); j--) {
            reg[j] = reg[j - 1];
        }
        reg[j] = r;
    }
    d->n = 0;

    float pox, poy;
    slate_el_parent_origin(root, &pox, &poy);
    frame_list(&cx, root, pox, poy);
    const slate_gfx_ops_t *prev = slate_raster_ops(NULL);
    for (int i = 0; i < nr; i++) {
        slate_rect_t R = reg[i];
        int rw = R.x1 - R.x0;
        int rows = imax(t->rows, (int)((long)t->rows * t->w / rw));   /* a narrow region: as many rows as fit */
        for (int y = R.y0; y < R.y1; y += rows) {
            int n = imin(rows, R.y1 - y), bi = band_next;
            band_next ^= 1;
            if (t->wait_free) {
                t->wait_free(t->ctx, bi);
            }
            uint16_t *px = bufs[bi];
            slate_buf_t band = { px, rw, n, rw, R.x0, y };
            cx.ops = slate_gfx();
            cx.ar_top = 0;
            cx.it_n = 0;
            render(&cx, &band, root, pox, poy, (slate_rect_t){ R.x0, y, R.x1, y + n }, false, true);
            slate_swap565_row(px, px, rw * n);
            emit(ctx, R, y, n, px);
            st->area += (uint32_t)(rw * n);
        }
    }
    slate_raster_ops(prev);
}
