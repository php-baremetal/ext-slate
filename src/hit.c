/* Slate's hit table: see hit.h. */
#include <math.h>
#include <string.h>

#include "bm_events_hal.h"
#include "hit.h"

/* ---- the published block ---- */

/* A table's memory: entries for cap, from the scene allocator. Outgrown blocks go on the retired list until the
   pub is freed (a reader may still be reading one: it then fails its seq check, but reads no freed memory). */
typedef struct hit_block {
    struct hit_block *next;   /* retired list */
    int32_t cap;              /* immutable */
    int32_t pad;
    slate_hit_t e[];
} hit_block_t;

struct slate_hit_pub {
    uint32_t seq;             /* atomic: tables published; t[seq & 1] is the one shown */
    int refs;                 /* atomic */
    struct {
        hit_block_t *blk;     /* atomic (relaxed): written by the writer only while the table is the free one */
        int32_t n;            /* atomic (relaxed): likewise */
    } t[2];
    hit_block_t *retired;     /* writer only */
};

slate_hit_pub_t *slate_hit_pub_new(void)
{
    /* internal RAM: atomics do not work in PSRAM on every chip (as slate_scroll_pub_t) */
    slate_hit_pub_t *p = bm_hal_alloc(sizeof *p, BM_HAL_MEM_INTERNAL);
    if (p) {
        memset(p, 0, sizeof *p);
        __atomic_store_n(&p->refs, 1, __ATOMIC_SEQ_CST);
    }
    return p;
}

void slate_hit_pub_ref(slate_hit_pub_t *p)
{
    if (p) {
        __atomic_add_fetch(&p->refs, 1, __ATOMIC_SEQ_CST);
    }
}

void slate_hit_pub_put(slate_hit_pub_t *p)
{
    if (!p || __atomic_sub_fetch(&p->refs, 1, __ATOMIC_SEQ_CST) != 0) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        slate_scene_mem_free(p->t[i].blk);
    }
    for (hit_block_t *b = p->retired; b;) {
        hit_block_t *next = b->next;
        slate_scene_mem_free(b);
        b = next;
    }
    bm_hal_free(p);
}

int slate_hit_pub_refs(const slate_hit_pub_t *p)
{
    return __atomic_load_n(&p->refs, __ATOMIC_SEQ_CST);
}

size_t slate_hit_table_bytes(int cap)
{
    return sizeof(hit_block_t) + (size_t)cap * sizeof(slate_hit_t);
}

slate_hit_t *slate_hit_pub_table(slate_hit_pub_t *p, int need)
{
    int i = (int)((__atomic_load_n(&p->seq, __ATOMIC_RELAXED) + 1u) & 1u);   /* the free one (only we write seq) */
    hit_block_t *b = p->t[i].blk;
    if (need > SLATE_HIT_MAX) {
        need = SLATE_HIT_MAX;
    }
    if (!b || b->cap < need) {
        int cap = b ? b->cap : SLATE_HIT_CAP_MIN;
        while (cap < need) {
            cap *= 2;
        }
        hit_block_t *nb = slate_scene_mem(slate_hit_table_bytes(cap));
        if (!nb) {
            return NULL;
        }
        nb->cap = cap;
        if (b) {
            memcpy(nb->e, b->e, (size_t)b->cap * sizeof(slate_hit_t));   /* what the writer wrote so far stays */
            b->next = p->retired;
            p->retired = b;
        }
        __atomic_store_n(&p->t[i].blk, nb, __ATOMIC_RELAXED);
        b = nb;
    }
    /* The table about to be written was the one shown until the last publish: a reader that copies an entry written
       from here on must see that publish's seq when it reads seq again (it then starts over). */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return b->e;
}

void slate_hit_pub_publish(slate_hit_pub_t *p, int n)
{
    uint32_t s = __atomic_load_n(&p->seq, __ATOMIC_RELAXED) + 1u;
    int i = (int)(s & 1u);
    hit_block_t *b = p->t[i].blk;
    if (n < 0 || !b) {
        n = 0;
    } else if (n > b->cap) {
        n = b->cap;
    }
    __atomic_store_n(&p->t[i].n, n, __ATOMIC_RELAXED);
    __atomic_store_n(&p->seq, s, __ATOMIC_RELEASE);   /* the entries and n before seq */
    __atomic_thread_fence(__ATOMIC_RELEASE);          /* and seq before whatever is written next */
}

/* The table of snapshot s: its entries and count (cut to its block: a reader late on seq may see a count and a
   block of two writes; it then fails its seq check). */
static const slate_hit_t *snap(const slate_hit_pub_t *p, uint32_t s, int *n)
{
    int i = (int)(s & 1u);
    const hit_block_t *b = __atomic_load_n(&p->t[i].blk, __ATOMIC_RELAXED);
    int k = __atomic_load_n(&p->t[i].n, __ATOMIC_RELAXED);
    if (!b || k < 0) {
        k = 0;
    } else if (k > b->cap) {
        k = b->cap;
    }
    *n = k;
    return b ? b->e : NULL;
}

/* After reading a snapshot: true when it was not replaced meanwhile. */
static bool still(const slate_hit_pub_t *p, uint32_t s)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&p->seq, __ATOMIC_RELAXED) == s;
}

static inline bool contains(const slate_hit_t *h, int x, int y)
{
    return x >= h->x0 && x < h->x1 && y >= h->y0 && y < h->y1;
}

int slate_hit_find(slate_hit_pub_t *p, int x, int y, slate_hit_t *out, uint32_t *seq)
{
    for (;;) {
        uint32_t s = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        int n, found = -1;
        const slate_hit_t *t = snap(p, s, &n);
        slate_hit_t h = { 0 };
        for (int i = n - 1; i >= 0; i--) {
            h = t[i];
            if (contains(&h, x, y)) {
                found = i;
                break;
            }
        }
        if (still(p, s)) {
            if (found >= 0 && out) {
                *out = h;
            }
            if (seq) {
                *seq = s;
            }
            return found;
        }
    }
}

int slate_hit_lookup(slate_hit_pub_t *p, uint32_t id, slate_hit_t *out, uint32_t *seq)
{
    for (;;) {
        uint32_t s = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        int n, found = -1;
        const slate_hit_t *t = snap(p, s, &n);
        slate_hit_t h = { 0 };
        for (int i = n - 1; i >= 0; i--) {
            h = t[i];
            if (h.id == id) {
                found = i;
                break;
            }
        }
        if (still(p, s)) {
            if (found >= 0 && out) {
                *out = h;
            }
            if (seq) {
                *seq = s;
            }
            return found;
        }
    }
}

bool slate_hit_at(slate_hit_pub_t *p, uint32_t seq, int i, slate_hit_t *out)
{
    uint32_t s = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
    if (s != seq) {
        return false;
    }
    int n;
    const slate_hit_t *t = snap(p, s, &n);
    if (i < 0 || i >= n) {
        return false;
    }
    slate_hit_t h = t[i];
    if (!still(p, s)) {
        return false;
    }
    *out = h;
    return true;
}

int slate_hit_copy(slate_hit_pub_t *p, slate_hit_t *out, int max, uint32_t *seq)
{
    for (;;) {
        uint32_t s = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        int n;
        const slate_hit_t *t = snap(p, s, &n);
        if (n > 0 && max > 0) {
            memcpy(out, t, (size_t)(n < max ? n : max) * sizeof *out);
        }
        if (still(p, s)) {
            if (seq) {
                *seq = s;
            }
            return n;
        }
    }
}

/* ---- the ids of the touchables ---- */

static uint32_t slot_of(const slate_el_t *el, uint32_t cap)
{
    uintptr_t v = (uintptr_t)el >> 3;
    return ((uint32_t)v * 2654435761u ^ (uint32_t)(v >> 16)) & (cap - 1);
}

static bool ids_grow(slate_hit_ids_t *m, uint32_t cap)
{
    const slate_el_t **el = slate_scene_mem(cap * sizeof *el);
    uint32_t *id = slate_scene_mem(cap * sizeof *id);
    if (!el || !id) {
        slate_scene_mem_free(el);
        slate_scene_mem_free(id);
        return false;
    }
    for (uint32_t i = 0; i < m->cap; i++) {
        if (m->el[i]) {
            uint32_t j = slot_of(m->el[i], cap);
            while (el[j]) {
                j = (j + 1) & (cap - 1);
            }
            el[j] = m->el[i];
            id[j] = m->id[i];
        }
    }
    slate_scene_mem_free(m->el);
    slate_scene_mem_free(m->id);
    m->el = el;
    m->id = id;
    m->cap = cap;
    return true;
}

bool slate_hit_ids_put(slate_hit_ids_t *m, const slate_el_t *el, uint32_t id)
{
    if (m->cap) {
        for (uint32_t j = slot_of(el, m->cap); m->el[j]; j = (j + 1) & (m->cap - 1)) {
            if (m->el[j] == el) {
                m->id[j] = id;
                return true;
            }
        }
    }
    if ((m->n + 1) * 2 > m->cap && !ids_grow(m, m->cap ? m->cap * 2 : 32)) {   /* at most half full */
        return false;
    }
    uint32_t j = slot_of(el, m->cap);
    while (m->el[j]) {
        j = (j + 1) & (m->cap - 1);
    }
    m->el[j] = el;
    m->id[j] = id;
    m->n++;
    return true;
}

void slate_hit_ids_del(slate_hit_ids_t *m, const slate_el_t *el)
{
    if (!m->cap) {
        return;
    }
    uint32_t mask = m->cap - 1, j = slot_of(el, m->cap);
    while (m->el[j] && m->el[j] != el) {
        j = (j + 1) & mask;
    }
    if (!m->el[j]) {
        return;
    }
    m->el[j] = NULL;
    m->n--;
    /* backward shift: the entries after it in its run move into the hole when their home slot allows it (no
       tombstones) */
    for (uint32_t k = (j + 1) & mask; m->el[k]; k = (k + 1) & mask) {
        uint32_t h = slot_of(m->el[k], m->cap);
        if (((k - h) & mask) >= ((k - j) & mask)) {   /* its home is at or before the hole */
            m->el[j] = m->el[k];
            m->id[j] = m->id[k];
            m->el[k] = NULL;
            j = k;
        }
    }
}

uint32_t slate_hit_ids_get(const slate_hit_ids_t *m, const slate_el_t *el)
{
    if (!m->cap) {
        return 0;
    }
    for (uint32_t j = slot_of(el, m->cap); m->el[j]; j = (j + 1) & (m->cap - 1)) {
        if (m->el[j] == el) {
            return m->id[j];
        }
    }
    return 0;
}

void slate_hit_ids_free(slate_hit_ids_t *m)
{
    slate_scene_mem_free(m->el);
    slate_scene_mem_free(m->id);
    memset(m, 0, sizeof *m);
}

/* ---- the walk ---- */

static const slate_rect_t NONE = { 0, 0, 0, 0 };

static inline slate_rect_t r_and(slate_rect_t a, slate_rect_t b)
{
    slate_rect_t r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
                       a.y1 < b.y1 ? a.y1 : b.y1 };
    return slate_rect_empty(r) ? NONE : r;
}

static inline slate_rect_t r_or(slate_rect_t a, slate_rect_t b)
{
    if (slate_rect_empty(a)) {
        return b;
    }
    if (slate_rect_empty(b)) {
        return a;
    }
    return (slate_rect_t){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
                           a.y1 > b.y1 ? a.y1 : b.y1 };
}

static inline slate_rect_t r_grow(slate_rect_t r, int d)
{
    return slate_rect_empty(r) ? NONE : (slate_rect_t){ r.x0 - d, r.y0 - d, r.x1 + d, r.y1 + d };
}

static inline bool hidden(const slate_el_t *e)
{
    return !e->visible || e->alpha == 0;
}

static inline int16_t i16(float v)
{
    float r = floorf(v + 0.5f);
    return (int16_t)(r < -32767.0f ? -32767 : r > 32767.0f ? 32767 : (int)r);
}

/* The screen origin of e's children given e's parent origin (unplaced). */
static void child_origin(const slate_el_t *e, float pox, float poy, float *cx, float *cy)
{
    slate_el_place(e, &pox, &poy);
    int dx, dy;
    slate_el_scroll_px(e, &dx, &dy);
    *cx = pox + e->x - (float)dx;
    *cy = poy + e->y - (float)dy;
}

/* e's extent on screen (e visible) given its parent origin: its geometry without shadow, blur or AA fringe; a group:
   its visible descendants'; a group that clips (a scroll among them): its clip box (its view).
   Cost: a touchable plain group walks its whole visible subtree (not pruned), and nested touchable plain groups walk
   theirs again: O(elements x depth) per build in the worst case. Fine for buttons and cards (2.6 us for 50 buttons on
   the host); a large touchable plain group pays it at every rebuild (accepted, see the board measurement). */
static slate_rect_t extent(slate_el_t *e, float pox, float poy)
{
    if (e->kind != SLATE_EL_GROUP) {
        return r_grow(slate_el_screen_ext(e, pox, poy, true), -SLATE_AA_MARGIN);
    }
    if (slate_el_clips(e)) {   /* a box of its own: a touch zone, a card, a scroll's view */
        slate_rect_t clip = slate_el_clip_rect(e, pox, poy);
        return slate_rect_empty(clip) ? NONE : clip;
    }
    float cx, cy;
    child_origin(e, pox, poy, &cx, &cy);
    slate_rect_t u = NONE;
    for (slate_el_t *k = e->u.g.first; k; k = k->next) {
        if (!hidden(k)) {
            u = r_or(u, extent(k, cx, cy));
        }
    }
    return u;
}

typedef struct {
    slate_hit_pub_t *p;
    const slate_hit_ids_t *ids;
    slate_hit_t *t;
    int cap, n;
    bool nomem;
} walk_t;

/* e (visible) and its subtree, given its parent origin, the clip of its ancestors and the entry of the nearest
   touchable scroll among them. */
static void walk(walk_t *w, slate_el_t *e, float pox, float poy, slate_rect_t clip, int scroll)
{
    bool group = e->kind == SLATE_EL_GROUP;
    if (e->touchable) {
        slate_rect_t box = r_and(r_grow(extent(e, pox, poy), e->hit_pad), clip);
        uint32_t id = slate_hit_ids_get(w->ids, e);
        if (!slate_rect_empty(box) && id && w->n < SLATE_HIT_MAX) {
            if (w->n == w->cap) {
                int cap = w->cap * 2 > SLATE_HIT_MAX ? SLATE_HIT_MAX : w->cap * 2;
                slate_hit_t *t = slate_hit_pub_table(w->p, cap);
                if (!t) {
                    w->nomem = true;
                    return;
                }
                w->t = t;
                w->cap = cap;
            }
            float px = pox, py = poy;
            slate_el_place(e, &px, &py);
            slate_hit_t h = { .id = id, .x0 = (int16_t)box.x0, .y0 = (int16_t)box.y0, .x1 = (int16_t)box.x1,
                              .y1 = (int16_t)box.y1, .ox = i16(px + e->x), .oy = i16(py + e->y),
                              .scroll = (int16_t)scroll, .flags = 0 };
            slate_scroll_t *s = group ? slate_el_scroll(e) : NULL;
            if (s) {
                float mx, my;
                slate_scroll_max(s, &mx, &my);
                h.flags = (uint8_t)(SLATE_HIT_SCROLL | (s->axis != SLATE_SCROLL_V && mx > 0 ? SLATE_HIT_AXIS_X : 0) |
                                    (s->axis != SLATE_SCROLL_H && my > 0 ? SLATE_HIT_AXIS_Y : 0));
                scroll = w->n;   /* its descendants' nearest touchable scroll */
            }
            w->t[w->n++] = h;
        }
    }
    if (!group || slate_el_touchables(e) <= (uint32_t)e->touchable) {
        return;   /* no touchable under it */
    }
    /* Pruned only by clips (a group's own clip, the screen): what a branch draws does not bound its hit boxes (a
       touch zone, a clipping group that draws nothing, has its clip box); subtrees without touchables are skipped */
    if (slate_el_clips(e)) {
        clip = r_and(clip, slate_el_clip_rect(e, pox, poy));
        if (slate_rect_empty(clip)) {
            return;
        }
    }
    float cx, cy;
    child_origin(e, pox, poy, &cx, &cy);
    for (slate_el_t *k = e->u.g.first; k && !w->nomem; k = k->next) {
        if (!hidden(k) && slate_el_touchables(k) > 0) {
            walk(w, k, cx, cy, clip, scroll);
        }
    }
}

int slate_hit_build(slate_hit_pub_t *p, slate_el_t *root, int sw, int sh, const slate_hit_ids_t *ids)
{
    uint32_t want = root ? slate_el_touchables(root) : 0;
    if (want == 0) {
        slate_hit_pub_publish(p, 0);
        return 0;
    }
    int cap = want > SLATE_HIT_MAX ? SLATE_HIT_MAX : (int)want;   /* touch_n bounds the entries (one per touchable) */
    slate_hit_t *t = slate_hit_pub_table(p, cap);
    if (!t) {
        return -1;
    }
    walk_t w = { .p = p, .ids = ids, .t = t, .cap = cap };
    if (!hidden(root)) {
        float ox, oy;
        slate_el_parent_origin(root, &ox, &oy);
        walk(&w, root, ox, oy, (slate_rect_t){ 0, 0, sw, sh }, -1);
    }
    if (w.nomem) {
        return -1;   /* nothing published: the table shown stays */
    }
    slate_hit_pub_publish(p, w.n);
    return w.n;
}
