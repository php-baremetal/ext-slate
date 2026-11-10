/* Slate's command buffer: see cmd.h. */
#include <string.h>

#include "bm_events_hal.h"
#include "cmd.h"
#include "halpool.h"

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

static void batch_free(batch_t *bt)
{
    if (bt) {
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

slate_cmd_result_t slate_cmd_push(slate_cmdbuf_t *b, const slate_cmd_t *c)
{
    if (c->kind == SLATE_CMD_PIXELS && pixels_bytes(c) == 0) {
        return SLATE_CMD_BAD_PIXELS;
    }
    if (!b->pending && !(b->pending = batch_new())) {
        return SLATE_CMD_NO_MEM;
    }
    batch_t *bt = b->pending;
    /* What it points to, measured first: nothing is appended unless all of it fits. */
    size_t text = 0, data = 0, grad = 0;
    int ns = 0;
    if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_TEXT && c->v.v.text) {
        text = strlen(c->v.v.text) + 1;
    }
    if (c->kind == SLATE_CMD_SET && c->v.type == SLATE_VAL_PAINT && c->v.v.paint.grad) {
        ns = c->v.v.paint.grad->ns > 0 && c->v.v.paint.grad->stops ? c->v.v.paint.grad->ns : 0;
        grad = align8(sizeof(slate_paint_grad_t)) + align8((size_t)ns * sizeof(slate_stop_t));
    }
    if (c->kind == SLATE_CMD_SHOW && c->data) {
        data = strlen(c->data) + 1;
    } else if (c->kind == SLATE_CMD_PIXELS) {
        data = pixels_bytes(c);
    }
    size_t need = bt->used + align8(text) + grad + align8(data);
    if (!grow((void **)&bt->cmds, &bt->cap, bt->n + 1, sizeof(slate_cmd_t), CMDS_MIN)
        || !grow((void **)&bt->bytes, &bt->bcap, need, 1, BYTES_MIN)) {
        return SLATE_CMD_NO_MEM;
    }
    slate_cmd_t *d = &bt->cmds[bt->n++];
    *d = *c;
    if (c->kind != SLATE_CMD_SHOW && c->kind != SLATE_CMD_PIXELS) {
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
    if (data) {
        d->data = put(bt, c->data, data);
    } else if (d->kind == SLATE_CMD_SHOW || d->kind == SLATE_CMD_PIXELS) {
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
        }
        c->data = resolve(bt, c->data);
    }
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
    bt->n = 0;
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
