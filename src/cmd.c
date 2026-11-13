/* Slate's command buffer: see cmd.h. */
#include <string.h>

#include "bm_events_hal.h"
#include "cmd.h"
#include "halpool.h"
#include "images.h"

#define KEEP_BATCHES 4          /* empty batches kept for reuse */
#define KEEP_BYTES 16384        /* a batch whose payload grew past this goes back without its payload memory */
#define KEEP_CMDS 256           /* likewise for its command array */
#define CMDS_MIN 32
#define BYTES_MIN 512

typedef struct batch {
    struct batch *next;
    slate_cmd_t *cmds;
    size_t n, cap;
    uint8_t *bytes;             /* payloads; pointers in cmds are offsets + 1 until the batch is taken */
    size_t used, bcap;
    bool resolved;              /* taken: the pointers in cmds point into bytes */
} batch_t;

struct slate_cmdbuf {
    bm_hal_lock_t *lock;
    batch_t *pending;           /* PHP side; NULL until the first push after a deliver that found no spare */
    batch_t *head, *tail;       /* delivered, oldest first (lock) */
    batch_t *spare;             /* empty batches (lock) */
    int nspare;
    batch_t *taken;             /* compositor side */
};

static void *mem(size_t n)
{
    return bm_hal_alloc(n, BM_HAL_MEM_EXTERNAL);
}

/* The image and scroll pub references the batch's commands hold (taken at push), given back. */
static void drop_images(batch_t *bt)
{
    for (size_t i = 0; i < bt->n; i++) {
        const slate_cmd_t *c = &bt->cmds[i];
        if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_IMAGE) {
            slate_image_put(c->v.v.image);
        } else if (c->kind == SLATE_CMD_SCROLL && c->data) {
            const slate_scroll_cmd_t *sc = bt->resolved ? c->data : (const void *)(bt->bytes + ((uintptr_t)c->data - 1));
            slate_scroll_pub_put(sc->pub);
        }
    }
    bt->n = 0;
    bt->resolved = false;
}

static void batch_free(batch_t *bt)
{
    if (bt) {
        drop_images(bt);
        bm_hal_free(bt->cmds);
        bm_hal_free(bt->bytes);
        bm_hal_free(bt);
    }
}

static batch_t *batch_new(void)
{
    batch_t *bt = mem(sizeof *bt);
    if (bt) {
        memset(bt, 0, sizeof *bt);
    }
    return bt;
}

slate_cmdbuf_t *slate_cmdbuf_new(void)
{
    slate_cmdbuf_t *b = mem(sizeof *b);
    if (!b) {
        return NULL;
    }
    memset(b, 0, sizeof *b);
    b->lock = slate_lock_get();
    if (!b->lock) {
        bm_hal_free(b);
        return NULL;
    }
    return b;
}

static void list_free(batch_t *bt)
{
    while (bt) {
        batch_t *next = bt->next;
        batch_free(bt);
        bt = next;
    }
}

void slate_cmdbuf_free(slate_cmdbuf_t *b)
{
    if (!b) {
        return;
    }
    batch_free(b->pending);
    batch_free(b->taken);
    list_free(b->head);
    list_free(b->spare);
    slate_lock_put(b->lock);
    bm_hal_free(b);
}

/* ---- PHP side ---- */

static bool grow(void **p, size_t *cap, size_t need, size_t unit, size_t min)
{
    if (need <= *cap) {
        return true;
    }
    size_t n = *cap ? *cap : min;
    while (n < need) {
        n *= 2;
    }
    void *q = mem(n * unit);
    if (!q) {
        return false;
    }
    if (*p) {
        memcpy(q, *p, *cap * unit);
        bm_hal_free(*p);
    }
    *p = q;
    *cap = n;
    return true;
}

static size_t align8(size_t n)
{
    return (n + 7) & ~(size_t)7;
}

/* Copies n bytes into the payload (room already made) and returns the offset + 1 as a pointer. */
static const void *put(batch_t *bt, const void *src, size_t n)
{
    size_t off = bt->used;
    memcpy(bt->bytes + off, src, n);
    bt->used = align8(off + n);
    return (const void *)(uintptr_t)(off + 1);
}

/* The bytes of a PIXELS command's pixels; 0 when the command is not acceptable. */
static size_t pixels_bytes(const slate_cmd_t *c)
{
    const slate_rect_t r = c->r;
    if (!c->data || r.x0 < 0 || r.y0 < 0 || r.x0 >= r.x1 || r.y0 >= r.y1 || r.x1 > 32767 || r.y1 > 32767) {
        return 0;
    }
    size_t n = (size_t)(r.x1 - r.x0) * (size_t)(r.y1 - r.y0) * 2;
    return n <= SLATE_CMD_PIXELS_MAX ? n : 0;
}

/* The bytes of a TWEEN's or EASE's spec (a TWEEN: up to its last property) or of a SCROLL's data; 0 when the command
   is not acceptable. */
static size_t spec_bytes(const slate_cmd_t *c)
{
    if (!c->data) {
        return 0;
    }
    if (c->kind == SLATE_CMD_EASE) {
        return sizeof(slate_ease_spec_t);
    }
    if (c->kind == SLATE_CMD_SCROLL) {
        const slate_scroll_cmd_t *s = c->data;
        bool ok = s->op < SLATE_SCROLL_OP_COUNT && (s->op != SLATE_SCROLL_OP_CONFIG || s->axis <= SLATE_SCROLL_H);
        return ok ? sizeof(slate_scroll_cmd_t) : 0;
    }
    const slate_tween_spec_t *t = c->data;
    if (t->n < 1 || t->n > SLATE_TWEEN_PROPS_MAX) {
        return 0;
    }
    return offsetof(slate_tween_spec_t, props) + t->n * sizeof(slate_tween_prop_t);
}

bool slate_trans_ok(const slate_trans_t *t)
{
    if (t->kind == SLATE_TRANS_NONE) {
        return true;
    }
    return t->kind < SLATE_TRANS_COUNT && (t->kind == SLATE_TRANS_FADE || t->dir < SLATE_DIR_COUNT) &&
           t->ease < SLATE_EASE_COUNT && t->ms >= 1;
}

slate_cmd_result_t slate_cmd_push(slate_cmdbuf_t *b, const slate_cmd_t *c)
{
    if (c->kind == SLATE_CMD_PIXELS && pixels_bytes(c) == 0) {
        return SLATE_CMD_BAD_PIXELS;
    }
    bool spec = c->kind == SLATE_CMD_TWEEN || c->kind == SLATE_CMD_EASE || c->kind == SLATE_CMD_SCROLL;
    if (spec && spec_bytes(c) == 0) {
        return SLATE_CMD_BAD_ARG;
    }
    if (c->kind == SLATE_CMD_SHOW && !slate_trans_ok(&c->tr)) {
        return SLATE_CMD_BAD_ARG;
    }
    if (!b->pending && !(b->pending = batch_new())) {
        return SLATE_CMD_NO_MEM;
    }
    batch_t *bt = b->pending;
    /* What it points to, measured first: nothing is appended unless all of it fits. */
    size_t text = 0, data = 0, grad = 0, phys = 0;
    int ns = 0;
    if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_TEXT && c->v.v.text) {
        text = strlen(c->v.v.text) + 1;
    }
    if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_PAINT && c->v.v.paint.grad) {
        ns = c->v.v.paint.grad->ns > 0 && c->v.v.paint.grad->stops ? c->v.v.paint.grad->ns : 0;
        grad = align8(sizeof(slate_paint_grad_t)) + align8((size_t)ns * sizeof(slate_stop_t));
    }
    if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_SCROLL && c->v.v.scroll) {
        phys = sizeof(slate_scroll_t);
    }
    if (c->kind == SLATE_CMD_SHOW && c->data) {
        data = strlen(c->data) + 1;
    } else if (c->kind == SLATE_CMD_PIXELS) {
        data = pixels_bytes(c);
    } else if (spec) {
        data = spec_bytes(c);
    }
    size_t need = bt->used + align8(text) + grad + align8(phys) + align8(data);
    if (!grow((void **)&bt->cmds, &bt->cap, bt->n + 1, sizeof(slate_cmd_t), CMDS_MIN)
        || !grow((void **)&bt->bytes, &bt->bcap, need, 1, BYTES_MIN)) {
        return SLATE_CMD_NO_MEM;
    }
    slate_cmd_t *d = &bt->cmds[bt->n++];
    *d = *c;
    if (c->kind != SLATE_CMD_SHOW && c->kind != SLATE_CMD_PIXELS && !spec) {
        d->data = NULL;
    }
    if (text) {
        d->v.v.text = put(bt, c->v.v.text, text);
    }
    if (grad) {
        slate_paint_grad_t g = *c->v.v.paint.grad;
        size_t goff = bt->used;
        bt->used += align8(sizeof g);
        g.stops = ns ? put(bt, c->v.v.paint.grad->stops, (size_t)ns * sizeof(slate_stop_t)) : NULL;
        g.ns = ns;
        memcpy(bt->bytes + goff, &g, sizeof g);
        d->v.v.paint.grad = (const void *)(uintptr_t)(goff + 1);
    }
    if (phys) {
        d->v.v.scroll = put(bt, c->v.v.scroll, phys);
    }
    if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_IMAGE) {
        slate_image_ref(c->v.v.image);   /* the image lives while the command does */
    }
    if (c->kind == SLATE_CMD_SCROLL) {
        slate_scroll_pub_ref(((const slate_scroll_cmd_t *)c->data)->pub);   /* likewise its pub */
    }
    if (data) {
        d->data = put(bt, c->data, data);
    } else if (d->kind == SLATE_CMD_SHOW || d->kind == SLATE_CMD_PIXELS || spec) {
        d->data = NULL;
    }
    return SLATE_CMD_OK;
}

bool slate_cmd_deliver(slate_cmdbuf_t *b)
{
    batch_t *bt = b->pending;
    if (!bt || bt->n == 0) {
        return false;
    }
    bt->next = NULL;
    bm_hal_lock_enter(b->lock);
    if (b->tail) {
        b->tail->next = bt;
    } else {
        b->head = bt;
    }
    b->tail = bt;
    batch_t *s = b->spare;
    if (s) {
        b->spare = s->next;
        b->nspare--;
    }
    bm_hal_lock_exit(b->lock);
    if (s) {
        s->next = NULL;
    }
    b->pending = s;   /* NULL: the next push makes one */
    return true;
}

size_t slate_cmd_pending(const slate_cmdbuf_t *b)
{
    return b->pending ? b->pending->n : 0;
}

/* ---- compositor side ---- */

static const void *resolve(const batch_t *bt, const void *p)
{
    return p ? bt->bytes + ((uintptr_t)p - 1) : NULL;
}

size_t slate_cmd_take(slate_cmdbuf_t *b, const slate_cmd_t **out)
{
    *out = NULL;
    if (b->taken) {
        return 0;   /* release the previous one first */
    }
    bm_hal_lock_enter(b->lock);
    batch_t *bt = b->head;
    if (bt) {
        b->head = bt->next;
        if (!b->head) {
            b->tail = NULL;
        }
    }
    bm_hal_lock_exit(b->lock);
    if (!bt) {
        return 0;
    }
    bt->next = NULL;
    for (size_t i = 0; i < bt->n; i++) {
        slate_cmd_t *c = &bt->cmds[i];
        if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_TEXT) {
            c->v.v.text = resolve(bt, c->v.v.text);
        } else if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_PAINT && c->v.v.paint.grad) {
            slate_paint_grad_t *g = (slate_paint_grad_t *)resolve(bt, c->v.v.paint.grad);
            g->stops = resolve(bt, g->stops);
            c->v.v.paint.grad = g;
        } else if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_SCROLL) {
            c->v.v.scroll = resolve(bt, c->v.v.scroll);
        }
        c->data = resolve(bt, c->data);
    }
    bt->resolved = true;
    b->taken = bt;
    *out = bt->cmds;
    return bt->n;
}

void slate_cmd_release(slate_cmdbuf_t *b)
{
    batch_t *bt = b->taken;
    if (!bt) {
        return;
    }
    b->taken = NULL;
    drop_images(bt);
    bt->used = 0;
    if (bt->bcap > KEEP_BYTES) {
        bm_hal_free(bt->bytes);   /* a big canvas update: not kept */
        bt->bytes = NULL;
        bt->bcap = 0;
    }
    if (bt->cap > KEEP_CMDS) {
        bm_hal_free(bt->cmds);   /* a huge batch: its command array is not kept either */
        bt->cmds = NULL;
        bt->cap = 0;
    }
    bool kept = false;
    bm_hal_lock_enter(b->lock);
    if (b->nspare < KEEP_BATCHES) {
        bt->next = b->spare;
        b->spare = bt;
        b->nspare++;
        kept = true;
    }
    bm_hal_lock_exit(b->lock);
    if (!kept) {
        batch_free(bt);
    }
}

/* ---- scroll pubs ---- */

slate_scroll_pub_t *slate_scroll_pub_new(void)
{
    /* internal RAM: atomics do not work in PSRAM on every chip */
    slate_scroll_pub_t *p = bm_hal_alloc(sizeof *p, BM_HAL_MEM_INTERNAL);
    if (p) {
        memset(p, 0, sizeof *p);
        __atomic_store_n(&p->refs, 1, __ATOMIC_SEQ_CST);
    }
    return p;
}

void slate_scroll_pub_ref(slate_scroll_pub_t *p)
{
    if (p) {
        __atomic_add_fetch(&p->refs, 1, __ATOMIC_SEQ_CST);
    }
}

void slate_scroll_pub_put(slate_scroll_pub_t *p)
{
    if (p && __atomic_sub_fetch(&p->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        bm_hal_free(p);
    }
}

void slate_scroll_pub_pos(const slate_scroll_pub_t *p, int *x, int *y)
{
    uint32_t w = __atomic_load_n(&p->pos, __ATOMIC_SEQ_CST);
    *x = (int16_t)(uint16_t)(w & 0xFFFFu);
    *y = (int16_t)(uint16_t)(w >> 16);
}

bool slate_scroll_pub_moving(const slate_scroll_pub_t *p)
{
    return __atomic_load_n(&p->moving, __ATOMIC_SEQ_CST) != 0;
}

uint32_t slate_scroll_pub_seq(const slate_scroll_pub_t *p)
{
    return __atomic_load_n(&p->seq, __ATOMIC_SEQ_CST);
}
