/* Scene graph of Slate: see scene.h. */
#include "slate_speed.h"   /* first: run for every change of every frame */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "scene.h"
#include "slate_config.h"

#define FBIG 8388608.0f

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
static inline int16_t i16(int v) { return (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v); }

static const slate_rect_t EMPTY = { 0, 0, 0, 0 };

/* ---- memory ---- */

static void *(*mem_alloc)(size_t) = malloc;
static void (*mem_free)(void *) = free;

void slate_scene_set_alloc(void *(*alloc)(size_t bytes), void (*free_fn)(void *p))
{
    mem_alloc = alloc && free_fn ? alloc : malloc;
    mem_free = alloc && free_fn ? free_fn : free;
}

void *slate_scene_mem(size_t n)
{
    void *p = mem_alloc(n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

void slate_scene_mem_free(void *p)
{
    if (p) {
        mem_free(p);
    }
}

/* ---- change clock ---- */

static uint32_t scene_clock;

uint32_t slate_scene_clock(void) { return scene_clock; }
uint32_t slate_scene_tick(void) { return ++scene_clock; }

static void touch(slate_el_t *e)
{
    e->mod_self = scene_clock;
    for (; e; e = e->parent) {
        e->mod_sub = scene_clock;
    }
}

/* ---- effects that are built ---- */

static slate_shadow_t *shadow_ptr(slate_el_t *e)
{
    switch (e->kind) {
    case SLATE_EL_ROUNDRECT: case SLATE_EL_CIRCLE: return &e->u.shape.shadow;
    case SLATE_EL_GROUP: return &e->u.g.shadow;
    default: return NULL;
    }
}

const slate_shadow_t *slate_el_shadow(const slate_el_t *e)
{
    return shadow_ptr((slate_el_t *)e);
}

int slate_el_blur(const slate_el_t *e)
{
#if SLATE_HAS_BLUR
    return e->kind == SLATE_EL_GROUP ? e->u.g.blur : 0;
#else
    (void)e;
    return 0;
#endif
}

bool slate_el_has_shadow(const slate_el_t *e)
{
#if SLATE_HAS_SHADOWS
    const slate_shadow_t *s = slate_el_shadow(e);
    return s && (s->argb >> 24) != 0;
#else
    (void)e;
    return false;
#endif
}

bool slate_el_snaps(const slate_el_t *e)
{
    return slate_el_has_shadow(e) || (e->kind == SLATE_EL_GROUP && (e->cached || slate_el_blur(e) > 0));
}

void slate_el_place(const slate_el_t *e, float *pox, float *poy)
{
    if (slate_el_snaps(e)) {
        *pox = (float)iround(*pox + e->x) - e->x;
        *poy = (float)iround(*poy + e->y) - e->y;
    }
}

/* ---- life ---- */

slate_el_t *slate_el_new(slate_el_kind_t kind)
{
    if (kind < SLATE_EL_GROUP || kind > SLATE_EL_CANVAS) {
        return NULL;
    }
    slate_el_t *e = slate_scene_mem(sizeof *e);
    if (!e) {
        return NULL;
    }
    e->kind = (uint8_t)kind;
    e->mod_self = e->mod_sub = scene_clock;
    e->visible = true;
    e->alpha = 255;
    switch (kind) {
    case SLATE_EL_ARC: e->u.arc.width = 1.0f; break;
    case SLATE_EL_LINE: e->u.line.width = 1.0f; e->u.line.argb = 0xFFFFFFFFu; break;
    case SLATE_EL_TEXT: e->u.text.argb = 0xFFFFFFFFu; break;
    default: break;
    }
    return e;
}

static void invalidate_up(slate_el_t *e)
{
    for (; e; e = e->parent) {
        e->ext_ok = false;
    }
}

static void detach(slate_el_t *c)
{
    slate_el_t *p = c->parent;
    if (!p) {
        return;
    }
    if (c->prev) {
        c->prev->next = c->next;
    } else {
        p->u.g.first = c->next;
    }
    if (c->next) {
        c->next->prev = c->prev;
    } else {
        p->u.g.last = c->prev;
    }
    c->parent = c->prev = c->next = NULL;
    touch(p);
    invalidate_up(p);
    slate_cache_stale(p, true, true);
    slate_cache_stale_up(p);
}

void slate_el_free(slate_el_t *e)
{
    if (!e) {
        return;
    }
    detach(e);
    switch (e->kind) {
    case SLATE_EL_GROUP:
        while (e->u.g.first) {
            slate_el_t *c = e->u.g.first;
            detach(c);
            slate_el_free(c);
        }
        break;
    case SLATE_EL_ROUNDRECT: case SLATE_EL_CIRCLE:
        slate_scene_mem_free((void *)e->u.shape.fill.grad);
        break;
    case SLATE_EL_ARC:
        slate_scene_mem_free((void *)e->u.arc.color.grad);
        break;
    case SLATE_EL_TEXT:
        slate_scene_mem_free(e->u.text.text);
        break;
    case SLATE_EL_CANVAS:
        slate_scene_mem_free(e->u.canvas.px);
        break;
    default:
        break;
    }
    slate_cache_drop(e);
    slate_scene_mem_free(e);
}

/* ---- geometry ---- */

/* Screen origin of a's children: a's own (x, y) on screen, placed. */
static void origin_of(const slate_el_t *a, float *ox, float *oy)
{
    float px = 0, py = 0;
    if (a->parent) {
        origin_of(a->parent, &px, &py);
    }
    slate_el_place(a, &px, &py);
    *ox = px + a->x;
    *oy = py + a->y;
}

void slate_el_parent_origin(const slate_el_t *e, float *ox, float *oy)
{
    *ox = *oy = 0;
    if (e->parent) {
        origin_of(e->parent, ox, oy);
    }
}

static bool hidden(const slate_el_t *e)
{
    return !e->visible || e->alpha == 0;
}

static bool hidden_chain(const slate_el_t *e)
{
    for (; e; e = e->parent) {
        if (hidden(e)) {
            return true;
        }
    }
    return false;
}

typedef struct { float x0, y0, x1, y1; } frect_t;
static const frect_t FEMPTY = { 0, 0, -1, -1 };
static bool fempty(frect_t r) { return !(r.x0 < r.x1 && r.y0 < r.y1); }
static frect_t frect_join(frect_t a, frect_t b)
{
    if (fempty(a)) return b;
    if (fempty(b)) return a;
    return (frect_t){ fmn(a.x0, b.x0), fmn(a.y0, b.y0), fmx(a.x1, b.x1), fmx(a.y1, b.y1) };
}

static void arc_point(frect_t *r, float cx, float cy, float rad, float deg, float grow)
{
    float a = deg * 3.14159265358979f / 180.0f;
    float px = cx + rad * cosf(a), py = cy + rad * sinf(a);
    frect_t p = { px - grow, py - grow, px + grow, py + grow };   /* grow 0: a point, which frect_join would drop */
    *r = fempty(*r) ? p : (frect_t){ fmn(r->x0, p.x0), fmn(r->y0, p.y0), fmx(r->x1, p.x1), fmx(r->y1, p.y1) };
}

static frect_t arc_ext(const slate_el_t *e)
{
    const slate_arc_t *a = &e->u.arc;
    float hw = a->width * 0.5f, ro = a->r + hw;
    float sweep = a->to - a->from;
    if (!(a->r > 0) || !(a->width > 0) || sweep == 0 || !isfinite(sweep)) {
        return FEMPTY;
    }
    if (fabsf(sweep) >= 360.0f) {
        return (frect_t){ e->x - ro, e->y - ro, e->x + ro, e->y + ro };
    }
    sweep = fmodf(sweep, 360.0f);
    if (sweep < 0) {
        sweep += 360.0f;
    }
    frect_t r = FEMPTY;
    arc_point(&r, e->x, e->y, a->r, a->from, hw);
    arc_point(&r, e->x, e->y, a->r, a->from + sweep, hw);
    for (int k = 0; k < 4; k++) {
        float d = fmodf((float)(k * 90) - a->from, 360.0f);
        if (d < 0) {
            d += 360.0f;
        }
        if (d <= sweep) {
            arc_point(&r, e->x, e->y, ro, (float)(k * 90), 0.0f);
        }
    }
    return r;
}

static frect_t shadow_of(const slate_el_t *e, frect_t c)
{
    if (fempty(c) || !slate_el_has_shadow(e)) {
        return FEMPTY;
    }
    const slate_shadow_t *s = slate_el_shadow(e);
    float g = (float)SLATE_BLUR_EXTENT(s->blur);
    return (frect_t){ c.x0 - g + s->dx, c.y0 - g + s->dy, c.x1 + g + s->dx, c.y1 + g + s->dy };
}

static frect_t content_ext(slate_el_t *e);

/* full extent (content and shadow), parent coordinates */
static frect_t full_ext(slate_el_t *e)
{
    frect_t c = content_ext(e);
    return frect_join(c, shadow_of(e, c));
}

static frect_t content_ext(slate_el_t *e)
{
    if (e->ext_ok) {
        return (frect_t){ e->cx0, e->cy0, e->cx1, e->cy1 };
    }
    frect_t c = FEMPTY;
    switch (e->kind) {
    case SLATE_EL_ROUNDRECT:
        if (e->u.shape.w > 0 && e->u.shape.h > 0) {
            c = (frect_t){ e->x, e->y, e->x + e->u.shape.w, e->y + e->u.shape.h };
        }
        break;
    case SLATE_EL_CIRCLE: {
        float r = e->u.shape.r;
        if (r > 0) {
            c = (frect_t){ e->x - r, e->y - r, e->x + r, e->y + r };
        }
        break;
    }
    case SLATE_EL_ARC:
        c = arc_ext(e);
        break;
    case SLATE_EL_LINE: {
        const slate_line_t *l = &e->u.line;
        if (l->width > 0) {
            float hw = l->width * 0.5f;
            c = (frect_t){ fmn(e->x, l->x1) + 0.5f - hw, fmn(e->y, l->y1) + 0.5f - hw, fmx(e->x, l->x1) + 0.5f + hw,
                           fmx(e->y, l->y1) + 0.5f + hw };
        }
        break;
    }
    case SLATE_EL_TEXT: {
        const slate_text_t *t = &e->u.text;
        if (t->bx0 < t->bx1 && t->by0 < t->by1) {
            c = (frect_t){ e->x + t->bx0, e->y + t->by0, e->x + t->bx1, e->y + t->by1 };
        }
        break;
    }
    case SLATE_EL_CANVAS:
        if (e->u.canvas.px) {
            c = (frect_t){ e->x, e->y, e->x + (float)e->u.canvas.w, e->y + (float)e->u.canvas.h };
        }
        break;
    case SLATE_EL_GROUP: {
        const slate_group_t *g = &e->u.g;
        for (slate_el_t *k = g->first; k; k = k->next) {
            if (hidden(k)) {
                continue;
            }
            frect_t f = full_ext(k);
            if (!fempty(f)) {
                /* a child placed on whole pixels moves by up to half a pixel from where its extent says: widen
                   it by that, so the shifts of any number of nested snapped levels stay inside this box */
                float p = slate_el_snaps(k) ? 0.5f : 0.0f;
                c = frect_join(c, (frect_t){ f.x0 + e->x - p, f.y0 + e->y - p, f.x1 + e->x + p, f.y1 + e->y + p });
            }
        }
        if (!fempty(c) && e->clip && g->w > 0 && g->h > 0) {
            c = (frect_t){ fmx(c.x0, e->x), fmx(c.y0, e->y), fmn(c.x1, e->x + g->w), fmn(c.y1, e->y + g->h) };
        }
        if (!fempty(c) && slate_el_blur(e) > 0) {
            float gr = (float)SLATE_BLUR_EXTENT(slate_el_blur(e));
            c = (frect_t){ c.x0 - gr, c.y0 - gr, c.x1 + gr, c.y1 + gr };
        }
        break;
    }
    }
    if (fempty(c) || !isfinite(c.x0 + c.y0 + c.x1 + c.y1)) {
        c = FEMPTY;
    }
    e->cx0 = c.x0, e->cy0 = c.y0, e->cx1 = c.x1, e->cy1 = c.y1;
    e->ext_ok = true;
    return c;
}

slate_rect_t slate_el_screen_ext(slate_el_t *e, float pox, float poy, bool content)
{
    frect_t r = content ? content_ext(e) : full_ext(e);
    if (fempty(r)) {
        return EMPTY;
    }
    slate_el_place(e, &pox, &poy);
    return (slate_rect_t){ ifloor(r.x0 + pox) - SLATE_AA_MARGIN, ifloor(r.y0 + poy) - SLATE_AA_MARGIN,
                           iceil(r.x1 + pox) + SLATE_AA_MARGIN, iceil(r.y1 + poy) + SLATE_AA_MARGIN };
}

slate_rect_t slate_el_clip_rect(const slate_el_t *g, float pox, float poy)
{
    slate_el_place(g, &pox, &poy);
    float x = pox + g->x, y = poy + g->y;
    return (slate_rect_t){ iround(x), iround(y), iround(x + g->u.g.w), iround(y + g->u.g.h) };
}

slate_rect_t slate_el_bbox(const slate_el_t *e)
{
    if (hidden_chain(e)) {
        return EMPTY;
    }
    float ox, oy;
    slate_el_parent_origin(e, &ox, &oy);
    return slate_el_screen_ext((slate_el_t *)e, ox, oy, false);
}

/* g is drawn through an image of its content (shadow, blur or cache): a change inside rebuilds the whole image, and
   a rebuilt image can differ from the one it replaces beyond the change (its colours are dithered where it was
   built, and it may have been built at another place), so the change concerns g's whole box. */
static bool has_effects(const slate_el_t *g)
{
    return slate_el_has_shadow(g) || slate_el_blur(g) > 0 || (g->kind == SLATE_EL_GROUP && g->cached);
}

bool slate_el_clips(const slate_el_t *g)
{
    return g->kind == SLATE_EL_GROUP && g->clip && g->u.g.w > 0 && g->u.g.h > 0;
}

static bool under_effects(const slate_el_t *e)
{
    for (const slate_el_t *a = e->parent; a; a = a->parent) {
        if (has_effects(a)) {
            return true;
        }
    }
    return false;
}

/* r (part of e's box) as seen through e's ancestors: cut by their clips, or grown to the box of one with effects. */
static slate_rect_t through_ancestors(const slate_el_t *e, slate_rect_t r)
{
    if (slate_rect_empty(r)) {
        return EMPTY;
    }
    for (const slate_el_t *a = e->parent; a; a = a->parent) {
        if (has_effects(a)) {
            r = slate_el_bbox(a);
        } else if (slate_el_clips(a)) {
            float ox, oy;
            slate_el_parent_origin(a, &ox, &oy);
            r = slate_rect_and(r, slate_el_clip_rect(a, ox, oy));
        }
    }
    return slate_rect_empty(r) ? EMPTY : r;
}

slate_rect_t slate_el_dirty_rect(const slate_el_t *e)
{
    return through_ancestors(e, slate_el_bbox(e));   /* the box is empty when hidden */
}

/* ---- derived state ---- */

static bool paint_opaque(slate_paint_t p)
{
    if (!p.grad) {
        return (p.argb >> 24) == 255;
    }
#if SLATE_HAS_GRADIENTS
    if (!p.grad->stops || p.grad->ns <= 0) {
        return false;
    }
    for (int i = 0; i < p.grad->ns && i < SLATE_GRADIENT_STOPS_MAX; i++) {
        if ((p.grad->stops[i].argb >> 24) != 255) {
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}

static void derive(slate_el_t *e)
{
    switch (e->kind) {
    case SLATE_EL_ROUNDRECT:
    case SLATE_EL_CIRCLE:
        e->opaque = e->alpha == 255 && paint_opaque(e->u.shape.fill);
        break;
    case SLATE_EL_CANVAS:
        e->opaque = e->alpha == 255 && e->u.canvas.px != NULL;
        break;
    default:
        e->opaque = false;
        break;
    }
    invalidate_up(e);
}

/* ---- tree ---- */

static void dirty(slate_dirty_t *d, slate_rect_t r, int sw, int sh)
{
    if (d && !slate_rect_empty(r)) {
        slate_dirty_add(d, r, sw, sh);
    }
}

/* A change under groups drawn through an image (has_effects) builds their images again, which may change them
   whole even when the element changed shows nothing (hidden, fully transparent): the outermost one's box. */
static void dirty_images(const slate_el_t *e, slate_dirty_t *d, int sw, int sh)
{
    const slate_el_t *top = NULL;
    for (const slate_el_t *a = e->parent; a; a = a->parent) {
        if (has_effects(a)) {
            top = a;
        }
    }
    if (d && top) {
        dirty(d, slate_el_dirty_rect(top), sw, sh);
    }
}

bool slate_el_add(slate_el_t *group, slate_el_t *child, slate_dirty_t *d, int sw, int sh)
{
    if (!group || !child || group->kind != SLATE_EL_GROUP || child->parent) {
        return false;
    }
    for (slate_el_t *a = group; a; a = a->parent) {
        if (a == child) {
            return false;
        }
    }
    slate_group_t *g = &group->u.g;
    child->parent = group;
    child->prev = g->last;
    child->next = NULL;
    if (g->last) {
        g->last->next = child;
    } else {
        g->first = child;
    }
    g->last = child;
    touch(group);
    touch(child);
    invalidate_up(group);
    slate_cache_stale_up(child);
    dirty(d, slate_el_dirty_rect(child), sw, sh);
    dirty_images(child, d, sw, sh);
    return true;
}

void slate_el_remove(slate_el_t *child, slate_dirty_t *d, int sw, int sh)
{
    if (!child || !child->parent) {
        return;
    }
    dirty(d, slate_el_dirty_rect(child), sw, sh);
    dirty_images(child, d, sw, sh);
    detach(child);
}

/* ---- properties ---- */

#define K(kind) (1u << (kind))
#define K_ALL (K(SLATE_EL_GROUP) | K(SLATE_EL_ROUNDRECT) | K(SLATE_EL_CIRCLE) | K(SLATE_EL_ARC) | K(SLATE_EL_LINE) | \
               K(SLATE_EL_TEXT) | K(SLATE_EL_CANVAS))
#define K_SHAPE (K(SLATE_EL_ROUNDRECT) | K(SLATE_EL_CIRCLE))

static unsigned prop_kinds(slate_prop_t p)
{
    switch (p) {
    case SLATE_PROP_X: case SLATE_PROP_Y: case SLATE_PROP_VISIBLE: case SLATE_PROP_OPACITY:
        return K_ALL;
    case SLATE_PROP_W: case SLATE_PROP_H:
        return K(SLATE_EL_ROUNDRECT) | K(SLATE_EL_CANVAS) | K(SLATE_EL_GROUP);
    case SLATE_PROP_FILL: case SLATE_PROP_BORDER: case SLATE_PROP_BORDER_COLOR:
        return K_SHAPE;
    case SLATE_PROP_RADIUS:
        return K(SLATE_EL_ROUNDRECT);
    case SLATE_PROP_R:
        return K(SLATE_EL_CIRCLE) | K(SLATE_EL_ARC);
    case SLATE_PROP_FROM: case SLATE_PROP_TO:
        return K(SLATE_EL_ARC);
    case SLATE_PROP_WIDTH: case SLATE_PROP_CAPS:
        return K(SLATE_EL_ARC) | K(SLATE_EL_LINE);
    case SLATE_PROP_COLOR:
        return K(SLATE_EL_ARC) | K(SLATE_EL_LINE) | K(SLATE_EL_TEXT);
    case SLATE_PROP_X1: case SLATE_PROP_Y1:
        return K(SLATE_EL_LINE);
    case SLATE_PROP_TEXT: case SLATE_PROP_FONT: case SLATE_PROP_ALIGN: case SLATE_PROP_MAX_WIDTH:
        return K(SLATE_EL_TEXT);
    case SLATE_PROP_CLIP: case SLATE_PROP_CACHE: case SLATE_PROP_BLUR:
        return K(SLATE_EL_GROUP);
    case SLATE_PROP_SHADOW:
        return K_SHAPE | K(SLATE_EL_GROUP);
    }
    return 0;
}

static bool type_ok(slate_prop_t p, slate_val_type_t t)
{
    switch (p) {
    case SLATE_PROP_VISIBLE: case SLATE_PROP_CLIP: case SLATE_PROP_CACHE:
        return t == SLATE_VAL_BOOL;
    case SLATE_PROP_FILL: case SLATE_PROP_COLOR: case SLATE_PROP_BORDER_COLOR:
        return t == SLATE_VAL_PAINT || t == SLATE_VAL_COLOR;
    case SLATE_PROP_CAPS: case SLATE_PROP_ALIGN:
        return t == SLATE_VAL_ENUM;
    case SLATE_PROP_TEXT:
        return t == SLATE_VAL_TEXT;
    case SLATE_PROP_FONT:
        return t == SLATE_VAL_FONT;
    case SLATE_PROP_SHADOW:
        return t == SLATE_VAL_SHADOW;
    default:
        return t == SLATE_VAL_NUM;
    }
}

/* The float a NUM property is stored in (NULL: stored otherwise). */
static float *num_field(slate_el_t *e, slate_prop_t p)
{
    bool g = e->kind == SLATE_EL_GROUP, rr = e->kind == SLATE_EL_ROUNDRECT, arc = e->kind == SLATE_EL_ARC;
    switch (p) {
    case SLATE_PROP_X: return &e->x;
    case SLATE_PROP_Y: return &e->y;
    case SLATE_PROP_W: return g ? &e->u.g.w : rr ? &e->u.shape.w : NULL;
    case SLATE_PROP_H: return g ? &e->u.g.h : rr ? &e->u.shape.h : NULL;
    case SLATE_PROP_BORDER: return &e->u.shape.border;
    case SLATE_PROP_RADIUS: return &e->u.shape.r;
    case SLATE_PROP_R: return arc ? &e->u.arc.r : &e->u.shape.r;
    case SLATE_PROP_FROM: return &e->u.arc.from;
    case SLATE_PROP_TO: return &e->u.arc.to;
    case SLATE_PROP_WIDTH: return arc ? &e->u.arc.width : &e->u.line.width;
    case SLATE_PROP_X1: return &e->u.line.x1;
    case SLATE_PROP_Y1: return &e->u.line.y1;
    default: return NULL;
    }
}

/* A copy of p (gradient and stops in one block); false when out of memory. */
static bool paint_copy(slate_paint_t p, slate_paint_t *out)
{
    out->argb = p.argb;
    out->grad = NULL;
    if (!p.grad) {
        return true;
    }
    int ns = p.grad->stops ? p.grad->ns : 0;
    ns = ns < 0 ? 0 : ns > SLATE_GRADIENT_STOPS_MAX ? SLATE_GRADIENT_STOPS_MAX : ns;
    struct own { slate_paint_grad_t g; slate_stop_t s[]; } *o =
        slate_scene_mem(sizeof *o + (size_t)ns * sizeof(slate_stop_t));
    if (!o) {
        return false;
    }
    o->g = *p.grad;
    if (ns) {
        memcpy(o->s, p.grad->stops, (size_t)ns * sizeof(slate_stop_t));
    }
    o->g.stops = o->s;
    o->g.ns = ns;
    out->grad = &o->g;
    return true;
}

/* Replaces *dst with a copy of the value's paint; *changed false when it is the same solid colour. */
static bool paint_set(slate_paint_t *dst, const slate_value_t *v, bool *changed)
{
    slate_paint_t p = { v->type == SLATE_VAL_COLOR ? v->v.argb : v->v.paint.argb, NULL };
    if (v->type == SLATE_VAL_PAINT && !paint_copy(v->v.paint, &p)) {
        return false;
    }
    *changed = p.grad || dst->grad || p.argb != dst->argb;
    if (*changed) {
        slate_scene_mem_free((void *)dst->grad);
        *dst = p;
    }
    return true;
}

static void text_layout(slate_el_t *e)
{
    slate_text_t *t = &e->u.text;
    slate_rect_t r = slate_text_bbox(0, 0, t->text, t->font, (slate_align_t)t->align, t->max_width);
    t->bx0 = i16(r.x0), t->by0 = i16(r.y0), t->bx1 = i16(r.x1), t->by1 = i16(r.y1);
}

static bool canvas_resize(slate_el_t *e, int w, int h)
{
    slate_canvas_t *c = &e->u.canvas;
    uint16_t *px = NULL;
    if (w > 0 && h > 0) {
        px = slate_scene_mem((size_t)w * (size_t)h * sizeof(uint16_t));
        if (!px) {
            return false;
        }
    }
    slate_scene_mem_free(c->px);
    c->px = px;
    c->w = (int16_t)w;
    c->h = (int16_t)h;
    return true;
}

static bool needs_cache(const slate_el_t *e)
{
    return (e->kind == SLATE_EL_GROUP && e->cached) || has_effects(e);
}

/* ---- the part of an arc that a change of from or to redraws ---- */

/* The sweep of an arc as slate_draw_arc() takes it: false when it is a full ring or draws nothing. */
static bool arc_sweep(const slate_arc_t *a, float *sweep)
{
    float span = a->to - a->from;
    if (!(a->r > 0) || !(a->width > 0) || !isfinite(span) || span >= 360.0f || span <= -360.0f) {
        return false;
    }
    float s = fmodf(span, 360.0f);
    if (s < 0) {
        s += 360.0f;
    }
    *sweep = s;
    return s != 0.0f;
}

/* Box of the ring sector of the arc e between the angles t0 <= t1 (degrees, t1 - t0 < 360), grown by m; parent
   coordinates. */
static frect_t arc_sector(const slate_el_t *e, float t0, float t1, float m)
{
    const slate_arc_t *a = &e->u.arc;
    float hw = a->width * 0.5f, ri = a->r - hw, ro = a->r + hw;
    frect_t r = FEMPTY;
    arc_point(&r, e->x, e->y, ri, t0, m);
    arc_point(&r, e->x, e->y, ro, t0, m);
    arc_point(&r, e->x, e->y, ri, t1, m);
    arc_point(&r, e->x, e->y, ro, t1, m);
    for (int k = 0; k < 4; k++) {
        float d = fmodf((float)(k * 90) - t0, 360.0f);
        if (d < 0) {
            d += 360.0f;
        }
        if (d <= t1 - t0) {
            arc_point(&r, e->x, e->y, ro, (float)(k * 90), m);
        }
    }
    return r;
}

/* The screen area that a change of the arc e's from or to (old: the arc before it) can change, cut by the
   ancestors: the ring sector between the old and the new end, grown by the cap and the anti-aliasing of both
   ends. The rasterizer (raster.c, arc_px) makes a pixel's coverage depend on an end only within that reach of the
   end's ray: on the far side of the centre the half planes are settled by the bisector, and where a line's
   extension passes near the other end (a sweep near 180 degrees) the other half plane is the smaller one there.
   arc_dirty_test checks it against the rasterizer. false: a full ring or an empty arc, before or after; then the
   whole box before and after is redrawn. */
static bool arc_change_rect(const slate_el_t *e, const slate_arc_t *old, slate_prop_t prop, slate_rect_t *out)
{
    const slate_arc_t *a = &e->u.arc;
    float w0, w1;
    if (!arc_sweep(old, &w0) || !arc_sweep(a, &w1)) {
        return false;
    }
    float m = (a->caps == SLATE_CAPS_ROUND ? a->width * 0.5f : 0.0f) + 2.0f;   /* cap, fringe and a pixel more */
    frect_t f;
    if (prop == SLATE_PROP_TO) {   /* the start stays: the ends at from + w0 and from + w1 */
        f = arc_sector(e, a->from + fmn(w0, w1), a->from + fmx(w0, w1), m);
    } else {   /* the end stays, up to rounding (its ray is redrawn too): the starts at end - w0 and end - w1 */
        float e0 = old->from + w0, e1 = a->from + w1;
        float de = fmodf(e1 - e0, 360.0f);
        de = de > 180.0f ? de - 360.0f : de < -180.0f ? de + 360.0f : de;
        f = frect_join(arc_sector(e, e0 - fmx(w0, w1), e0 - fmn(w0, w1), m),
                       arc_sector(e, e0 + fmn(de, 0.0f), e0 + fmx(de, 0.0f), m));
    }
    if (fempty(f) || !isfinite(f.x0 + f.y0 + f.x1 + f.y1)) {
        return false;
    }
    float ox, oy;
    slate_el_parent_origin(e, &ox, &oy);
    slate_el_place(e, &ox, &oy);
    slate_rect_t r = { ifloor(f.x0 + ox) - SLATE_AA_MARGIN, ifloor(f.y0 + oy) - SLATE_AA_MARGIN,
                       iceil(f.x1 + ox) + SLATE_AA_MARGIN, iceil(f.y1 + oy) + SLATE_AA_MARGIN };
    *out = through_ancestors(e, r);
    return true;
}

bool slate_el_set(slate_el_t *e, slate_prop_t prop, const slate_value_t *v, slate_dirty_t *d, int sw, int sh)
{
    if (!e || !v || !(prop_kinds(prop) & K(e->kind)) || !type_ok(prop, v->type)) {
        return false;
    }
    if (v->type == SLATE_VAL_NUM && !isfinite(v->v.num)) {
        return false;
    }
    float *f = num_field(e, prop);
    if (f && *f == v->v.num) {   /* unchanged: nothing to do (and nothing to redraw) */
        return true;
    }
    bool arc_end = e->kind == SLATE_EL_ARC && (prop == SLATE_PROP_FROM || prop == SLATE_PROP_TO);
    slate_arc_t arc_old = arc_end ? e->u.arc : (slate_arc_t){ 0 };
    /* A shape placed on whole pixels (it has a shadow) that stays on the same pixel: the shape and its shadow come
       from its rounded origin (slate_el_place), so the screen does not change. Not for a group, nor inside a group
       drawn through an image: the move rebuilds images, which may come out dithered differently (has_effects). */
    bool still = false;
    if (d && (prop == SLATE_PROP_X || prop == SLATE_PROP_Y) && e->kind != SLATE_EL_GROUP && slate_el_snaps(e) &&
        !under_effects(e)) {
        float ox, oy;
        slate_el_parent_origin(e, &ox, &oy);
        float o = prop == SLATE_PROP_X ? ox : oy;
        still = iround(o + *f) == iround(o + v->v.num);
    }
    slate_rect_t before = d && !still ? slate_el_dirty_rect(e) : EMPTY;
    bool own_content = false, own_shadow = false, changed = true;
    uint32_t argb = v->type == SLATE_VAL_COLOR ? v->v.argb : v->type == SLATE_VAL_PAINT ? v->v.paint.argb : 0;

    switch (prop) {
    case SLATE_PROP_X: case SLATE_PROP_Y:
    case SLATE_PROP_FROM: case SLATE_PROP_TO: case SLATE_PROP_X1: case SLATE_PROP_Y1:
        *f = v->v.num;
        break;
    case SLATE_PROP_W: case SLATE_PROP_H:
        if (e->kind == SLATE_EL_CANVAS) {
            int n = iround(v->v.num);
            n = n < 0 ? 0 : n > 32767 ? 32767 : n;
            int w = prop == SLATE_PROP_W ? n : e->u.canvas.w, h = prop == SLATE_PROP_H ? n : e->u.canvas.h;
            if (w == e->u.canvas.w && h == e->u.canvas.h) {
                return true;
            }
            if (!canvas_resize(e, w, h)) {
                return false;
            }
        } else {
            *f = v->v.num;
        }
        own_content = own_shadow = true;
        break;
    case SLATE_PROP_BORDER: case SLATE_PROP_RADIUS: case SLATE_PROP_R: case SLATE_PROP_WIDTH:
        *f = v->v.num;
        own_shadow = true;
        break;
    case SLATE_PROP_VISIBLE:
        changed = e->visible != v->v.b;
        e->visible = v->v.b;
        break;
    case SLATE_PROP_OPACITY: {
        float o = v->v.num < 0 ? 0 : v->v.num > 1 ? 1 : v->v.num;
        uint8_t a = (uint8_t)(o * 255.0f + 0.5f);
        changed = a != e->alpha;
        e->alpha = a;
        break;
    }
    case SLATE_PROP_FILL:
        if (!paint_set(&e->u.shape.fill, v, &changed)) {
            return false;
        }
        break;
    case SLATE_PROP_COLOR:
        if (e->kind == SLATE_EL_ARC) {
            if (!paint_set(&e->u.arc.color, v, &changed)) {
                return false;
            }
        } else {   /* LINE, TEXT: solid colour only, a gradient's argb is used */
            uint32_t *c = e->kind == SLATE_EL_LINE ? &e->u.line.argb : &e->u.text.argb;
            changed = *c != argb;
            *c = argb;
        }
        break;
    case SLATE_PROP_BORDER_COLOR:
        changed = e->u.shape.border_argb != argb;
        e->u.shape.border_argb = argb;
        break;
    case SLATE_PROP_CAPS: {
        uint8_t c = v->v.e == SLATE_CAPS_ROUND ? SLATE_CAPS_ROUND : SLATE_CAPS_BUTT;
        uint8_t *dst = e->kind == SLATE_EL_ARC ? &e->u.arc.caps : &e->u.line.caps;
        changed = *dst != c;
        *dst = c;
        break;
    }
    case SLATE_PROP_ALIGN: {
        uint8_t al = v->v.e == SLATE_ALIGN_CENTER ? SLATE_ALIGN_CENTER
                     : v->v.e == SLATE_ALIGN_RIGHT ? SLATE_ALIGN_RIGHT : SLATE_ALIGN_LEFT;
        changed = e->u.text.align != al;
        e->u.text.align = al;
        text_layout(e);
        break;
    }
    case SLATE_PROP_MAX_WIDTH: {
        int n = iround(v->v.num);
        int16_t mw = i16(n < 0 ? 0 : n);
        changed = mw != e->u.text.max_width;
        e->u.text.max_width = mw;
        text_layout(e);
        break;
    }
    case SLATE_PROP_TEXT: {
        const char *s = v->v.text ? v->v.text : "";
        const char *old = e->u.text.text ? e->u.text.text : "";
        if (strcmp(old, s) == 0) {
            return true;
        }
        size_t n = strlen(s) + 1;
        char *t = slate_scene_mem(n);
        if (!t) {
            return false;
        }
        memcpy(t, s, n);
        slate_scene_mem_free(e->u.text.text);
        e->u.text.text = t;
        text_layout(e);
        break;
    }
    case SLATE_PROP_FONT:
        changed = e->u.text.font != v->v.font;
        e->u.text.font = v->v.font;
        text_layout(e);
        break;
    case SLATE_PROP_CLIP:
        changed = e->clip != v->v.b;
        e->clip = v->v.b;
        own_content = true;
        break;
    case SLATE_PROP_CACHE:
        changed = e->cached != v->v.b;
        e->cached = v->v.b;
        break;
    case SLATE_PROP_BLUR: {
        int b = iround(v->v.num);
        b = b < 0 ? 0 : b > SLATE_BLUR_RADIUS_MAX ? SLATE_BLUR_RADIUS_MAX : b;
        changed = b != e->u.g.blur;
        e->u.g.blur = (int16_t)b;
        own_content = true;
        break;
    }
    case SLATE_PROP_SHADOW: {
        slate_shadow_t s = v->v.shadow, *dst = shadow_ptr(e);
        s.blur = (int16_t)(s.blur < 0 ? 0 : s.blur > SLATE_BLUR_RADIUS_MAX ? SLATE_BLUR_RADIUS_MAX : s.blur);
        if (!(s.argb >> 24)) {
            s = (slate_shadow_t){ 0, 0, 0, 0 };
        }
        changed = s.dx != dst->dx || s.dy != dst->dy || s.blur != dst->blur || s.argb != dst->argb;
        *dst = s;
        own_shadow = true;
        break;
    }
    }
    if (!changed) {
        return true;
    }

    derive(e);
    if (own_content || own_shadow) {
        slate_cache_stale(e, own_content, own_shadow);
    }
    if (e->cache && !needs_cache(e)) {
        slate_cache_drop(e);
    }
    touch(e);
    slate_cache_stale_up(e);
    dirty_images(e, d, sw, sh);
    slate_rect_t part;
    if (still) {
        return true;
    }
    if (d && arc_end && !slate_rect_empty(before) && !under_effects(e) && arc_change_rect(e, &arc_old, prop, &part)) {
        dirty(d, part, sw, sh);
    } else if (d) {
        dirty(d, before, sw, sh);
        dirty(d, slate_el_dirty_rect(e), sw, sh);
    }
    return true;
}

void slate_el_move(slate_el_t *e, const slate_value_t *x, const slate_value_t *y, slate_dirty_t *d, int sw, int sh,
                   bool *ok_x, bool *ok_y)
{
    bool num = e && x && y && x->type == SLATE_VAL_NUM && y->type == SLATE_VAL_NUM && isfinite(x->v.num) &&
               isfinite(y->v.num);
    if (!d || !num) {
        *ok_x = slate_el_set(e, SLATE_PROP_X, x, d, sw, sh);
        *ok_y = slate_el_set(e, SLATE_PROP_Y, y, d, sw, sh);
        return;
    }
    if (e->x == x->v.num || e->y == y->v.num) {
        /* one axis at most changes: one set does it (the other one changes nothing) */
        *ok_x = slate_el_set(e, SLATE_PROP_X, x, d, sw, sh);
        *ok_y = slate_el_set(e, SLATE_PROP_Y, y, d, sw, sh);
        return;
    }
    /* still: as in slate_el_set, for both axes at once (each of the two sets would be still) */
    bool still = false;
    if (e->kind != SLATE_EL_GROUP && slate_el_snaps(e) && !under_effects(e)) {
        float ox, oy;
        slate_el_parent_origin(e, &ox, &oy);
        still = iround(ox + e->x) == iround(ox + x->v.num) && iround(oy + e->y) == iround(oy + y->v.num);
    }
    slate_rect_t before = still ? EMPTY : slate_el_dirty_rect(e);
    *ok_x = slate_el_set(e, SLATE_PROP_X, x, NULL, sw, sh);
    *ok_y = slate_el_set(e, SLATE_PROP_Y, y, NULL, sw, sh);
    dirty_images(e, d, sw, sh);
    if (!still) {
        dirty(d, before, sw, sh);
        dirty(d, slate_el_dirty_rect(e), sw, sh);
    }
}

/* ---- canvas ---- */

uint16_t *slate_canvas_px(slate_el_t *e, int *w, int *h)
{
    if (!e || e->kind != SLATE_EL_CANVAS) {
        return NULL;
    }
    if (w) *w = e->u.canvas.px ? e->u.canvas.w : 0;
    if (h) *h = e->u.canvas.px ? e->u.canvas.h : 0;
    return e->u.canvas.px;
}

void slate_canvas_dirty(slate_el_t *e, const slate_rect_t *r, slate_dirty_t *d, int sw, int sh)
{
    if (!e || e->kind != SLATE_EL_CANVAS || !e->u.canvas.px) {
        return;
    }
    touch(e);
    slate_cache_stale_up(e);
    dirty_images(e, d, sw, sh);
    if (!d || hidden_chain(e)) {
        return;
    }
    slate_rect_t box = slate_el_bbox(e);
    if (r) {
        float ox, oy;
        slate_el_parent_origin(e, &ox, &oy);
        int X = iround(ox + e->x), Y = iround(oy + e->y);
        slate_rect_t c = slate_rect_and(*r, (slate_rect_t){ 0, 0, e->u.canvas.w, e->u.canvas.h });
        box = slate_rect_and(box, (slate_rect_t){ X + c.x0, Y + c.y0, X + c.x1, Y + c.y1 });
    }
    dirty(d, through_ancestors(e, box), sw, sh);
}
