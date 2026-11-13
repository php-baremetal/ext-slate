/* Element caches and the alpha-tracking graphics table of Slate: see cache.h. */
#include "slate_speed.h"   /* first: the code below is built for speed */
#include <string.h>

#include "cache.h"
#include "slate_config.h"

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

static inline unsigned div255(unsigned x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* ---- records ---- */

slate_cache_t *slate_cache_of(slate_el_t *e)
{
    if (!e->cache) {
        e->cache = slate_scene_mem(sizeof(slate_cache_t));
    }
    return e->cache;
}

void slate_cache_stale(slate_el_t *e, bool content, bool shadow)
{
    slate_cache_t *c = e->cache;
    if (!c) {
        return;
    }
    if (content) {
        c->content_ok = false;
        c->gave_up = false;
        c->shadow_ok = false;   /* a group's shadow comes from its content */
    }
    if (shadow) {
        c->shadow_ok = false;
    }
}

void slate_cache_stale_up(slate_el_t *e)
{
    for (slate_el_t *a = e->parent; a; a = a->parent) {
        slate_cache_stale(a, true, true);
    }
}

void slate_cache_drop(slate_el_t *e)
{
    slate_cache_t *c = e->cache;
    if (!c) {
        return;
    }
    if (c->free) {
        if (c->px) {
            c->free(c->ctx, c->px);
        }
        if (c->sa) {
            c->free(c->ctx, c->sa);
        }
    }
    slate_scene_mem_free(c);
    e->cache = NULL;
}

void slate_cache_drop_tree(slate_el_t *e)
{
    slate_cache_drop(e);
    if (e->kind == SLATE_EL_GROUP) {
        for (slate_el_t *k = e->u.g.first; k; k = k->next) {
            slate_cache_drop_tree(k);
        }
    }
}

void *slate_cache_mem(slate_cache_t *c, void **slot, size_t *have, size_t need, void *(*alloc)(void *ctx, size_t),
                      void (*free_fn)(void *ctx, void *p), void *ctx)
{
    if (*slot && *have >= need && *have <= need * 2) {
        return *slot;
    }
    if (*slot && c->free) {
        c->free(c->ctx, *slot);
    }
    *slot = NULL;
    *have = 0;
    if (!alloc || need == 0) {
        return NULL;
    }
    c->free = free_fn;
    c->ctx = ctx;
    *slot = alloc(ctx, need);
    if (*slot) {
        *have = need;
    }
    return *slot;
}

/* ---- alpha-tracking table ---- */

static inline uint8_t *aplane(slate_buf_t *b) { return ((slate_bufa_t *)b)->a; }

static slate_rect_t clip_buf(const slate_buf_t *b, slate_rect_t r)
{
    r.x0 = imax(r.x0, 0);
    r.y0 = imax(r.y0, 0);
    r.x1 = imin(r.x1, b->w);
    r.y1 = imin(r.y1, b->h);
    return r;
}

static inline void over(uint8_t *A, unsigned a)
{
    if (a != 0) {
        *A = (uint8_t)(a == 255 ? 255 : a + div255(*A * (255 - a)));
    }
}

static void a8_fill(slate_buf_t *b, slate_rect_t r, uint16_t c)
{
    slate_gfx()->fill(b, r, c);
    r = clip_buf(b, r);
    for (int y = r.y0; y < r.y1; y++) {
        if (r.x1 > r.x0) {
            memset(aplane(b) + (size_t)y * b->stride + r.x0, 255, (size_t)(r.x1 - r.x0));
        }
    }
}

static void a8_fill_a(slate_buf_t *b, slate_rect_t r, uint16_t c, uint8_t a)
{
    slate_gfx()->fill_a(b, r, c, a);
    r = clip_buf(b, r);
    for (int y = r.y0; y < r.y1; y++) {
        uint8_t *row = aplane(b) + (size_t)y * b->stride;
        for (int x = r.x0; x < r.x1; x++) {
            over(&row[x], a);
        }
    }
}

static void a8_fill_mask(slate_buf_t *b, slate_rect_t r, uint16_t c, uint8_t a, const uint8_t *cov, int cov_stride)
{
    slate_gfx()->fill_mask(b, r, c, a, cov, cov_stride);
    slate_rect_t cr = clip_buf(b, r);
    for (int y = cr.y0; y < cr.y1; y++) {
        uint8_t *row = aplane(b) + (size_t)y * b->stride;
        const uint8_t *m = cov + (size_t)(y - r.y0) * cov_stride;
        for (int x = cr.x0; x < cr.x1; x++) {
            over(&row[x], a == 255 ? m[x - r.x0] : div255((unsigned)m[x - r.x0] * a));
        }
    }
}

static int window(const slate_buf_t *b, int x, int y, int sw, int sh, slate_rect_t clip, slate_rect_t *out)
{
    slate_rect_t r = { imax(x, clip.x0), imax(y, clip.y0), imin(x + sw, clip.x1), imin(y + sh, clip.y1) };
    *out = clip_buf(b, r);
    return out->x0 < out->x1 && out->y0 < out->y1;
}

static void a8_copy(slate_buf_t *b, int x, int y, const uint16_t *src, int sw, int sh, int sstride, slate_rect_t clip)
{
    slate_gfx()->copy(b, x, y, src, sw, sh, sstride, clip);
    slate_rect_t r;
    if (!window(b, x, y, sw, sh, clip, &r)) {
        return;
    }
    for (int j = r.y0; j < r.y1; j++) {
        memset(aplane(b) + (size_t)j * b->stride + r.x0, 255, (size_t)(r.x1 - r.x0));
    }
}

static void a8_copy_a(slate_buf_t *b, int x, int y, const uint16_t *src, const uint8_t *alpha, int sw, int sh,
                      int sstride, uint8_t a, slate_rect_t clip)
{
    slate_gfx()->copy_a(b, x, y, src, alpha, sw, sh, sstride, a, clip);
    slate_rect_t r;
    if (a == 0 || !window(b, x, y, sw, sh, clip, &r)) {
        return;
    }
    for (int j = r.y0; j < r.y1; j++) {
        uint8_t *row = aplane(b) + (size_t)j * b->stride;
        const uint8_t *al = alpha + (size_t)(j - y) * sstride;
        for (int i = r.x0; i < r.x1; i++) {
            over(&row[i], a == 255 ? al[i - x] : div255((unsigned)al[i - x] * a));
        }
    }
}

#if SLATE_HAS_GRADIENTS
/* The gradient's alpha at t (straight stop alphas interpolated, as the colour layer does). */
static unsigned grad_alpha(float t, const slate_stop_t *st, int ns)
{
    if (t <= st[0].at) {
        return st[0].argb >> 24;
    }
    for (int i = 0; i + 1 < ns; i++) {
        if (t < st[i + 1].at) {
            float w = st[i + 1].at - st[i].at;
            float f = w > 0 ? (t - st[i].at) / w : 1.0f;
            float a = (float)(st[i].argb >> 24) + f * ((float)(st[i + 1].argb >> 24) - (float)(st[i].argb >> 24));
            return (unsigned)(a + 0.5f);
        }
    }
    return st[ns - 1].argb >> 24;
}

static void a8_gradient_row(slate_buf_t *b, int x, int y, int n, float t0, float dt, const slate_stop_t *stops, int ns,
                            const uint8_t *cov)
{
    slate_gfx()->gradient_row(b, x, y, n, t0, dt, stops, ns, cov);
    if (y < 0 || y >= b->h || ns <= 0 || n <= 0) {
        return;
    }
    if (ns > SLATE_GRADIENT_STOPS_MAX) {
        ns = SLATE_GRADIENT_STOPS_MAX;
    }
    bool opaque = true;
    for (int i = 0; i < ns; i++) {
        opaque = opaque && (stops[i].argb >> 24) == 255;
    }
    uint8_t *row = aplane(b) + (size_t)y * b->stride;
    for (int i = imax(0, -x), e = imin(n, b->w - x); i < e; i++) {
        unsigned a = opaque ? 255 : grad_alpha(t0 + (float)(x + i + b->ox) * dt, stops, ns);
        over(&row[x + i], cov ? div255(a * cov[i]) : a);
    }
}
#endif

static void a8_blur(uint16_t *px, uint8_t *alpha, int w, int h, int r, void *tmp)
{
    slate_gfx()->blur(px, alpha, w, h, r, tmp);
}

static const slate_gfx_ops_t a8_ops = {
    a8_fill, a8_fill_a, a8_fill_mask, a8_copy, a8_copy_a,
#if SLATE_HAS_GRADIENTS
    a8_gradient_row,
#else
    NULL,
#endif
#if SLATE_GFX_BLUR
    a8_blur,
#else
    NULL,
#endif
};

const slate_gfx_ops_t *slate_a8_ops(void)
{
#if !SLATE_GFX_BLUR
    (void)a8_blur;
#endif
    return &a8_ops;
}
