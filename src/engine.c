/* Slate's compositor task: see engine.h. */
#include <stdio.h>
#include <string.h>

#include "beam.h"
#include "bm_events_hal.h"
#include "cache.h"
#include "compose.h"
#include "debug.h"
#include "engine.h"
#include "halpool.h"
#include "images.h"
#include "scene.h"
#include "slate_config.h"

#if SLATE_DEBUG
#include "display_font.h"
#endif

enum { MSG_KICK = 1, MSG_VSYNC, MSG_STOP };          /* wake queue */
/* engine task -> PHP task. Values no other queue carries: the queues are recycled (halpool.h) and change role, so
   a late message for an old wake or freed queue (0..3) is never taken for one of these. */
enum { CTL_FAILED = 0x534C0A01, CTL_READY, CTL_JOINED };

#define ID_EMPTY 0u
#define ID_GONE UINT32_MAX     /* a deleted slot of the id table */
#define MAP_MIN 64
#define MIN_ROWS 8             /* band rows are halved down to this when buf_alloc refuses */
#define SECOND_US 1000000
#define WAIT_SLICE_MS 100      /* a wait for a band buffer re-checks its flag this often */
#define MSG_TIMER 2u           /* on the freed queue: the scan wait's timer (0 and 1 are buffers) */
#define CACHE_HDR 8            /* bytes before every cache block: its size */
#define RETRY_FRAMES 3         /* frames in a row that send again the bands whose push failed */
#define ERR_TIMEOUT "the panel did not finish a band within " SLATE_STR(SLATE_DONE_TIMEOUT_MS) " ms"
#define SLATE_STR(x) SLATE_STR2(x)
#define SLATE_STR2(x) #x

typedef struct { uint32_t id; slate_el_t *el; } map_ent_t;
/* A band buffer as the driver's done sees it. push_us and bytes: set by the engine before the push; svc_us and
   timed: by the done (the driver's task) before it frees the buffer, read by the engine once it is free. */
typedef struct {
    slate_engine_t *e;
    int i;
    int64_t push_us;
    uint32_t bytes;
    uint32_t svc_us;               /* from when the driver could start the band to its done */
    bool timed;                    /* svc_us not yet given to the transfer estimate */
} band_slot_t;

struct slate_engine {
    /* set at start, read-only after */
    const display_band_ops_t *ops;
    void *panel;
    int w, h;
    bool has_te;
    display_scan_t scan;           /* has_te and a known scan order: the bands race the scan (beam.h) */
    uint32_t period_us;            /* no-TE pacing and the frame budget */
    bm_hal_queue_t *wake, *freed, *ctl;
    bm_hal_lock_t *lock;           /* st, last_scene_us */
    slate_cmdbuf_t *cmds;

    /* shared, atomic */
    int kick_flag, vsync_flag, want_vsync, stop_flag;
    int paused;                    /* the panel sleeps: no frame (slate_engine_pause) */
    int redraw_flag;               /* the panel lost its picture: the next frame sends the whole screen */
    int busy[2];                   /* band buffer i handed to push, its done still to come */
    slate_te_t te;                 /* the TE pulses: written by the interrupt */
    size_t cache_budget;
    const char *error;             /* sticky: the engine stopped composing (slate_engine_error) */
    char error_text[64];           /* the error with a code (refused()): written once, before error points at it */

    /* under lock */
    slate_engine_stats_t st;
    int64_t last_scene_us;

    /* driver task only (the dones, one after the other) */
    int64_t drv_done_us;           /* the last done */

    /* engine task only */
    uint16_t *buf[2];
    bool lost[2];                  /* its done never came: never pushed or freed again (the compositor may still
                                      draw into it, so a driver that sends it late may show a garbled band) */
    band_slot_t slot[2];
    void *scratch;                 /* on demand (scratch_grow) */
    size_t scratch_bytes;
    size_t scratch_refused;        /* the last size internal RAM did not have, and when */
    int64_t scratch_refused_us;
    size_t scratch_need;           /* the most the frames asked for since scratch_full_us */
    int64_t scratch_full_us;       /* when the whole scratch was last asked for (or taken) */
    int rows;
    slate_target_t target;
    size_t cache_bytes;
    map_ent_t *map;
    uint32_t map_cap, map_used, map_live;   /* used: live + deleted slots */
    slate_el_t *screen, *scene;
    char scene_name[48];
    slate_dirty_t dirty;
    bool scene_dirty;              /* the dirty set holds a change of the scene (not only the FPS counter) */
    int64_t last_frame_us;         /* start of the last frame (no-TE pacing) */
    uint32_t wait_acc_us;          /* blocked on the panel during the frame in progress */
    slate_xfer_t xfer;             /* how long a band takes */
    int64_t ready_us;              /* when the driver has sent every band pushed so far (upper estimate) */
    bool race;                     /* the frame in progress races the scan */
    /* bands composed but not pushed yet, in order (see emit()); te: the pulse of their frame */
    struct { int i; slate_rect_t region; int y0, rows; int64_t te; } held[2];
    int held_n;
    int64_t frame_te_us;           /* the pulse that started the frame in progress: its bands go behind its scan */
    uint32_t beam_waits;           /* bands that waited for the scan */
    uint64_t beam_wait_us;
    uint32_t push_errors;
    int frame_ok, frame_refused;   /* pushes accepted and refused in the frame in progress */
    int refused_frames;            /* frames in a row that had pushes refused and none accepted */
    bm_err_t last_err;             /* the last code begin or push returned */
    bool drawn;                    /* a frame went to the panel (slate_engine_redraw has something to redo) */
    slate_dirty_t retry;           /* bands whose push failed in the frame in progress */
    int retry_frames;              /* frames in a row that had failed pushes */
    int64_t dirty_since_us;        /* with TE: the task started waiting for a pulse (fallback) */
    int64_t prev_scene_us;         /* start of the last scene frame (fps) */
    uint32_t images_seen;          /* slate_images_done() when the image elements were last synced */
    float fps;
#if SLATE_DEBUG
    slate_debug_t dbg;
    slate_el_t *ovl, *ovl_bg, *ovl_text;
    int ovl_fps;
    char dbg_line[200];            /* the statistics line still to print ("" none) */
#endif
};

static slate_engine_t *s_running;   /* PHP task only */
/* The timer that ends a wait for the scan, one for every engine (the events HAL cannot delete timers): it sends
   MSG_TIMER to s_timer_q, the freed queue of the engine that runs. */
static bm_hal_timer_t *s_timer;
static bm_hal_queue_t *s_timer_q;

static int64_t now_us(void)
{
    return bm_hal_now_us();
}

static int load(const int *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static void store(int *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static int swap(int *p, int v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

static void send_msg(bm_hal_queue_t *q, uint32_t m, uint32_t wait_ms)
{
    bm_hal_queue_send(q, &m, wait_ms);
}

/* ---- memory ---- */

static void *ext_alloc(size_t n)
{
    return bm_hal_alloc(n, BM_HAL_MEM_EXTERNAL);
}

static void ext_free(void *p)
{
    bm_hal_free(p);
}

/* Element caches: external RAM, refused past the budget. Each block carries its size in front (8 bytes, so the
   block stays 8-byte aligned). */
static void *cache_alloc(void *ctx, size_t n)
{
    slate_engine_t *e = ctx;
    size_t budget = __atomic_load_n(&e->cache_budget, __ATOMIC_RELAXED);
    if (n > budget || e->cache_bytes > budget - n) {
        return NULL;
    }
    uint8_t *p = bm_hal_alloc(n + CACHE_HDR, BM_HAL_MEM_EXTERNAL_ONLY);   /* never internal RAM */
    if (!p) {
        return NULL;
    }
    memcpy(p, &n, sizeof n);
    e->cache_bytes += n;
    return p + CACHE_HDR;
}

static void cache_free(void *ctx, void *q)
{
    slate_engine_t *e = ctx;
    if (!q) {
        return;
    }
    uint8_t *p = (uint8_t *)q - CACHE_HDR;
    size_t n;
    memcpy(&n, p, sizeof n);
    e->cache_bytes -= n;
    bm_hal_free(p);
}

/* Default cache budget: half of the largest external block found (probing from 2 * SLATE_CACHE_BUDGET_MAX down),
   at most SLATE_CACHE_BUDGET_MAX; 0 without external RAM (the probe never takes internal RAM). */
static size_t probe_budget(void)
{
    for (size_t n = 2 * (size_t)SLATE_CACHE_BUDGET_MAX; n >= 16384; n /= 2) {
        void *p = bm_hal_alloc(n, BM_HAL_MEM_EXTERNAL_ONLY);
        if (p) {
            bm_hal_free(p);
            return n / 2;
        }
    }
    return 0;
}

/* ---- the id table (open addressing, linear probing) ---- */

static uint32_t hash(uint32_t id)
{
    id ^= id >> 16;
    id *= 0x45d9f3bu;
    id ^= id >> 16;
    return id;
}

static map_ent_t *map_slot(slate_engine_t *e, uint32_t id)
{
    uint32_t mask = e->map_cap - 1;
    for (uint32_t i = hash(id) & mask;; i = (i + 1) & mask) {
        map_ent_t *m = &e->map[i];
        if (m->id == id || m->id == ID_EMPTY) {
            return m;
        }
    }
}

static slate_el_t *map_find(slate_engine_t *e, uint32_t id)
{
    if (id == ID_EMPTY || id == ID_GONE) {
        return NULL;
    }
    map_ent_t *m = map_slot(e, id);
    return m->id == id ? m->el : NULL;
}

static bool map_resize(slate_engine_t *e, uint32_t cap)
{
    map_ent_t *old = e->map;
    uint32_t ocap = e->map_cap;
    map_ent_t *m = ext_alloc(cap * sizeof *m);
    if (!m) {
        return false;
    }
    memset(m, 0, cap * sizeof *m);
    e->map = m;
    e->map_cap = cap;
    e->map_used = e->map_live;
    for (uint32_t i = 0; i < ocap; i++) {
        if (old[i].id != ID_EMPTY && old[i].id != ID_GONE) {
            *map_slot(e, old[i].id) = old[i];
        }
    }
    ext_free(old);
    return true;
}

/* id must not be in the table. */
static bool map_put(slate_engine_t *e, uint32_t id, slate_el_t *el)
{
    if ((e->map_used + 1) * 4 > e->map_cap * 3) {
        uint32_t cap = (e->map_live + 1) * 2 > e->map_cap ? e->map_cap * 2 : e->map_cap;   /* else: tombstones */
        if (!map_resize(e, cap)) {
            return false;
        }
    }
    uint32_t mask = e->map_cap - 1;
    for (uint32_t i = hash(id) & mask;; i = (i + 1) & mask) {
        map_ent_t *m = &e->map[i];
        if (m->id == ID_EMPTY || m->id == ID_GONE) {
            if (m->id == ID_EMPTY) {
                e->map_used++;
            }
            m->id = id;
            m->el = el;
            e->map_live++;
            return true;
        }
    }
}

static void map_del(slate_engine_t *e, uint32_t id)
{
    map_ent_t *m = map_slot(e, id);
    if (m->id == id) {
        m->id = ID_GONE;
        m->el = NULL;
        e->map_live--;
    }
}

/* ---- the band target ---- */

static uint16_t *band_buf(void *ctx, int i)
{
    return ((slate_engine_t *)ctx)->buf[i];
}

/* The driver's done (its task, never an ISR): buffer i is free. */
static void band_done(void *arg)
{
    band_slot_t *s = arg;
    slate_engine_t *e = s->e;
    bm_hal_queue_t *q = e->freed;
    uint32_t i = (uint32_t)s->i;
    int64_t t = now_us(), start = s->push_us > e->drv_done_us ? s->push_us : e->drv_done_us;
    s->svc_us = (uint32_t)(t - start);   /* the dones come in push order: the driver started it at the last one */
    s->timed = true;
    e->drv_done_us = t;
    store(&e->busy[i], 0);
    /* From here the engine may stop and be freed: only q is used below. That is safe because queues are never
       deleted, only recycled by halpool.h (a stale message in a recycled queue is harmless: a wait re-checks its
       flag, and the ctl messages are tagged). If the events HAL ever deletes queues, send before the store and let
       teardown drain the queue instead. */
    bm_hal_queue_send(q, &i, 0);
}

static void fail(slate_engine_t *e, const char *msg)
{
    const char *none = NULL;
    __atomic_compare_exchange_n(&e->error, &none, msg, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static bool failed(slate_engine_t *e)
{
    return __atomic_load_n(&e->error, __ATOMIC_SEQ_CST) != NULL;
}

/* The panel refused the bands for good (engine task): fails with the driver's code. */
static void refused(slate_engine_t *e, bm_err_t err)
{
    if (!failed(e)) {
        snprintf(e->error_text, sizeof e->error_text, "the panel refused the bands (error 0x%x)", (unsigned)err);
        fail(e, e->error_text);
    }
}

/* Waits for buffer i, at most SLATE_DONE_TIMEOUT_MS: then the buffer is lost (the driver may still read it: it is
   never pushed or freed again) and the engine fails. The compositor may still draw into it for the rest of the
   frame, but nothing reaches the panel any more. */
static void release_held(slate_engine_t *e, int upto);

static void wait_free(void *ctx, int i)
{
    slate_engine_t *e = ctx;
    for (int k = 0; k < e->held_n; k++) {
        if (e->held[k].i == i) {
            release_held(e, k);   /* the band in it is pushed first (and the ones before it) */
            break;
        }
    }
    if (e->lost[i] || !load(&e->busy[i])) {
        return;
    }
    int64_t t = now_us(), deadline = t + (int64_t)SLATE_DONE_TIMEOUT_MS * 1000;
    while (load(&e->busy[i])) {
        int64_t left = deadline - now_us();
        if (left <= 0) {
            e->lost[i] = true;
            fail(e, ERR_TIMEOUT);
            break;
        }
        uint32_t m, ms = left < (int64_t)WAIT_SLICE_MS * 1000 ? (uint32_t)((left + 999) / 1000) : WAIT_SLICE_MS;
        bm_hal_queue_receive(e->freed, &m, ms);   /* a message for either buffer: re-check the flag */
    }
    e->wait_acc_us += (uint32_t)(now_us() - t);
}

/* ---- racing the scan ---- */

static void timer_cb(void *arg)
{
    (void)arg;
    bm_hal_timer_stop(s_timer);   /* one shot */
    bm_hal_queue_t *q = __atomic_load_n(&s_timer_q, __ATOMIC_ACQUIRE);
    if (q) {
        uint32_t m = MSG_TIMER;
        bm_hal_queue_send(q, &m, 0);
    }
}

/* The band times measured since the last call go to the estimate. */
static void xfer_collect(slate_engine_t *e)
{
    for (int i = 0; i < 2; i++) {
        band_slot_t *s = &e->slot[i];
        if (!load(&e->busy[i]) && s->timed) {
            s->timed = false;
            slate_xfer_sample(&e->xfer, s->bytes, s->svc_us);
        }
    }
}

static bool any_busy(slate_engine_t *e)
{
    for (int i = 0; i < 2; i++) {
        if (load(&e->busy[i])) {
            return true;
        }
    }
    return false;
}

/* Sleeps until when (or a done: the caller looks again), on the timer when there is one, never longer than the
   time to when plus a tick of the queue's clock. */
static void sleep_until(slate_engine_t *e, int64_t when)
{
    int64_t d = when - now_us();
    if (d <= 0) {
        return;
    }
    if (s_timer) {
        bm_hal_timer_stop(s_timer);
        bm_hal_timer_start_us(s_timer, (uint64_t)d);
    }
    uint32_t m;
    bm_hal_queue_receive(e->freed, &m, (uint32_t)((d + 999) / 1000) + 1);   /* a done or the timer: both fine */
}

/* When a band of rows [y0, y0 + rows) of the region, of the frame that went behind the pulse frame_te (0: a frame
   that does not race), could be written without the scan crossing it, as things stand now. */
static int64_t beam_due(slate_engine_t *e, slate_rect_t region, int y0, int rows, int64_t frame_te)
{
    int a, b, lines;
    int64_t now = now_us(), te;
    uint32_t period;
    if (!frame_te || !slate_beam_span(e->scan, e->w, e->h, region.x0, region.x1, y0, rows, &a, &b, &lines) ||
        !slate_beam_model(&e->te, now, &te, &period)) {
        return now;
    }
    xfer_collect(e);
    uint32_t bytes = (uint32_t)rows * (uint32_t)(region.x1 - region.x0) * 2;
    int64_t ready = any_busy(e) ? e->ready_us : now;
    return slate_beam_when(te, period, a, b, lines, now, ready, slate_xfer_us(&e->xfer, bytes), frame_te);
}

/* Waits until a band of rows [y0, y0 + rows) of the region can be written without the scan crossing it. Nothing
   without TE, with an unknown scan order, or while the pulses do not give a steady period. */
static void beam_wait(slate_engine_t *e, slate_rect_t region, int y0, int rows, uint32_t bytes, int64_t frame_te)
{
    int a, b, lines;
    if (!frame_te || !slate_beam_span(e->scan, e->w, e->h, region.x0, region.x1, y0, rows, &a, &b, &lines)) {
        return;
    }
    int64_t t0 = now_us();
    for (;;) {   /* a done or a late timer message ends a sleep early: look again */
        xfer_collect(e);
        int64_t now = now_us(), te;
        uint32_t period;
        if (!slate_beam_model(&e->te, now, &te, &period) || now - t0 > 2 * (int64_t)period) {
            break;   /* no steady TE (any more), or a bound on the wait whatever happens */
        }
        int64_t ready = any_busy(e) ? e->ready_us : now;
        int64_t when = slate_beam_when(te, period, a, b, lines, now, ready, slate_xfer_us(&e->xfer, bytes),
                                       frame_te);
        if (when <= now) {
            break;
        }
        sleep_until(e, when);
    }
    int64_t waited = now_us() - t0;
    if (waited > 50) {
        if (s_timer) {
            bm_hal_timer_stop(s_timer);   /* woken early: no late message for the next wait */
        }
        e->beam_waits++;
        e->beam_wait_us += (uint64_t)waited;
    }
}

/* Pushes the band in buffer i (rows [y0, y0 + rows) of region), opening the region before its first band, once the
   scan is out of its way when the frame races, and ends the region after its last band. */
static void push_band(slate_engine_t *e, int i, slate_rect_t region, int y0, int rows, int64_t frame_te)
{
    const display_band_ops_t *ops = e->ops;
    if (failed(e) || e->lost[i]) {
        return;   /* the panel stopped answering: nothing more is sent */
    }
    int64_t t = now_us();
    if (y0 == region.y0) {
        bm_err_t err = ops->begin(e->panel, region.x0, region.y0, region.x1, region.y1);
        if (err == BM_ERR_INVALID_STATE) {
            refused(e, err);   /* the panel failed for good (display_band.h) */
            return;
        }
        if (err != BM_OK) {
            e->push_errors++;
            e->last_err = err;
        }
    }
    uint32_t bytes = (uint32_t)rows * (uint32_t)(region.x1 - region.x0) * 2;
    beam_wait(e, region, y0, rows, bytes, frame_te);
    band_slot_t *s = &e->slot[i];
    s->push_us = now_us();
    s->bytes = bytes;
    s->timed = false;
    store(&e->busy[i], 1);
    bm_err_t err = ops->push(e->panel, y0, rows, band_buf(e, i), band_done, s);
    if (err != BM_OK) {
        store(&e->busy[i], 0);   /* not taken: no done, the buffer is free at once */
        e->push_errors++;
        e->frame_refused++;
        e->last_err = err;
        if (err == BM_ERR_INVALID_STATE) {
            refused(e, err);   /* the panel failed for good (display_band.h) */
            return;
        }
        slate_dirty_add(&e->retry, (slate_rect_t){ region.x0, y0, region.x1, y0 + rows }, e->w, e->h);
    } else {
        e->frame_ok++;
        e->ready_us = (e->ready_us > s->push_us ? e->ready_us : s->push_us) + slate_xfer_us(&e->xfer, bytes);
    }
    if (y0 + rows == region.y1 && ops->end(e->panel) != BM_OK) {
        e->push_errors++;
    }
    e->wait_acc_us += (uint32_t)(now_us() - t);   /* push blocks while the driver is full */
}

/* The held bands up to held[upto] go out, in order, each once the scan is out of its way (see emit). */
static void release_held(slate_engine_t *e, int upto)
{
    int n = upto + 1;
    for (int k = 0; k < n; k++) {
        push_band(e, e->held[k].i, e->held[k].region, e->held[k].y0, e->held[k].rows, e->held[k].te);
    }
    e->held_n -= n;
    memmove(e->held, e->held + n, (size_t)e->held_n * sizeof e->held[0]);
}

/* The held bands whose time has come go out (none waits). */
static void release_due(slate_engine_t *e)
{
    int k = 0;
    while (k < e->held_n &&
           beam_due(e, e->held[k].region, e->held[k].y0, e->held[k].rows, e->held[k].te) <= now_us()) {
        k++;
    }
    if (k > 0) {
        release_held(e, k - 1);
    }
}

/* A band is composed. It goes to the panel in the order composed, after the ones before it. When the frame races
   the scan, a band whose rows the scan has not left yet is held: the compositor draws the next band into the other
   buffer meanwhile, and the held band goes out as soon as its time comes at a later emit, or before its buffer is
   drawn again, or at the end of the frame (waiting then for what is left of its wait). */
static void emit(void *ctx, slate_rect_t region, int y0, int rows, uint16_t *px)
{
    slate_engine_t *e = ctx;
    int i = px == e->buf[0] ? 0 : 1;
    if (!e->race) {
        push_band(e, i, region, y0, rows, 0);
        return;
    }
    e->held[e->held_n].i = i;
    e->held[e->held_n].region = region;
    e->held[e->held_n].y0 = y0;
    e->held[e->held_n].rows = rows;
    e->held[e->held_n].te = e->frame_te_us;
    e->held_n++;
    release_due(e);
}

/* ---- the scene ---- */

static bool shown(const slate_engine_t *e, const slate_el_t *el)
{
    while (el->parent) {
        el = el->parent;
    }
    return el == e->screen;
}

/* The dirty set for a change of el: NULL when el is not on screen. */
static slate_dirty_t *dirty_for(slate_engine_t *e, const slate_el_t *el)
{
    if (!shown(e, el)) {
        return NULL;
    }
    e->scene_dirty = true;
    return &e->dirty;
}

static void dirty_all(slate_engine_t *e)
{
    slate_dirty_add(&e->dirty, (slate_rect_t){ 0, 0, e->w, e->h }, e->w, e->h);
    e->scene_dirty = true;
}

static uint16_t rgb565(uint32_t argb)
{
    return (uint16_t)((((argb >> 19) & 0x1F) << 11) | (((argb >> 10) & 0x3F) << 5) | ((argb >> 3) & 0x1F));
}

#if SLATE_DEBUG
static void overlay_top(slate_engine_t *e)
{
    if (e->ovl) {
        slate_el_remove(e->ovl, NULL, e->w, e->h);
        slate_el_add(e->screen, e->ovl, NULL, e->w, e->h);
    }
}
#endif

static void scratch_drop(slate_engine_t *e);
static void scratch_trim(slate_engine_t *e, size_t want, int64_t now);

static bool apply_show(slate_engine_t *e, const slate_cmd_t *c)
{
    slate_el_t *s = c->id ? map_find(e, c->id) : NULL;
    if (c->id && (!s || s->kind != SLATE_EL_GROUP || (s->parent && s != e->scene))) {
        return false;
    }
    if (e->scene != s) {
        if (e->scene) {
            slate_el_remove(e->scene, NULL, e->w, e->h);
            slate_cache_drop_tree(e->scene);   /* its caches go: rebuilt when it is shown again */
            scratch_drop(e);   /* the new scene asks for what it needs */
        }
        e->scene = s;
        if (s) {
            slate_el_add(e->screen, s, NULL, e->w, e->h);
        }
#if SLATE_DEBUG
        overlay_top(e);
#endif
    }
    snprintf(e->scene_name, sizeof e->scene_name, "%s", c->data ? (const char *)c->data : "");
    e->target.background = c->v.type == SLATE_VAL_COLOR ? rgb565(c->v.v.argb) : 0;
    dirty_all(e);
    return true;
}

static bool apply_free(slate_engine_t *e, uint32_t id)
{
    slate_el_t *el = map_find(e, id);
    if (!el) {
        return false;
    }
    if (el == e->scene) {
        slate_el_remove(el, NULL, e->w, e->h);
        e->scene = NULL;
        dirty_all(e);
    }
    if (el->kind == SLATE_EL_GROUP) {
        while (el->u.g.first) {   /* the children have their own ids: they stay */
            slate_el_t *ch = el->u.g.first;
            slate_el_remove(ch, dirty_for(e, ch), e->w, e->h);
        }
    }
    slate_el_remove(el, dirty_for(e, el), e->w, e->h);
    slate_el_free(el);
    map_del(e, id);
    return true;
}

static bool apply_pixels(slate_engine_t *e, const slate_cmd_t *c)
{
    slate_el_t *el = map_find(e, c->id);
    int cw, ch;
    uint16_t *px = el && c->data ? slate_canvas_px(el, &cw, &ch) : NULL;
    if (!px) {
        return false;
    }
    slate_rect_t r = slate_rect_and(c->r, (slate_rect_t){ 0, 0, cw, ch });
    if (slate_rect_empty(r)) {
        return true;
    }
    const uint16_t *src = c->data;
    int sw = c->r.x1 - c->r.x0;
    for (int y = r.y0; y < r.y1; y++) {
        memcpy(px + (size_t)y * cw + r.x0, src + (size_t)(y - c->r.y0) * sw + (r.x0 - c->r.x0),
               (size_t)(r.x1 - r.x0) * 2);
    }
    slate_canvas_dirty(el, &r, dirty_for(e, el), e->w, e->h);
    return true;
}

static bool apply(slate_engine_t *e, const slate_cmd_t *c)
{
    slate_el_t *el, *g;
    switch (c->kind) {
    case SLATE_CMD_NEW:
        if (c->id == ID_EMPTY || c->id == ID_GONE || map_find(e, c->id)) {
            return false;
        }
        el = slate_el_new(c->el);
        if (!el) {
            return false;
        }
        if (!map_put(e, c->id, el)) {
            slate_el_free(el);
            return false;
        }
        return true;
    case SLATE_CMD_SET:
        el = map_find(e, c->id);
        return el && slate_el_set(el, c->prop, &c->v, dirty_for(e, el), e->w, e->h);
    case SLATE_CMD_ADD:
        g = map_find(e, c->parent);
        el = map_find(e, c->id);
        return g && el && slate_el_add(g, el, dirty_for(e, g), e->w, e->h);
    case SLATE_CMD_REMOVE:
        el = map_find(e, c->id);
        if (!el || el->parent == e->screen) {
            return false;   /* a scene root leaves the screen through SHOW */
        }
        slate_el_remove(el, el->parent ? dirty_for(e, el) : NULL, e->w, e->h);
        return true;
    case SLATE_CMD_FREE:
        return apply_free(e, c->id);
    case SLATE_CMD_SHOW:
        return apply_show(e, c);
    case SLATE_CMD_PIXELS:
        return apply_pixels(e, c);
    }
    return false;
}

static void publish(slate_engine_t *e, uint32_t batches, uint32_t cmds, uint32_t errors)
{
    bm_hal_lock_enter(e->lock);
    e->st.batches += batches;
    e->st.cmds += cmds;
    e->st.cmd_errors += errors;
    e->st.elements = e->map_live;
    e->st.cache_bytes = e->cache_bytes;
    bm_hal_lock_exit(e->lock);
}

static bool is_move(const slate_cmd_t *a, const slate_cmd_t *b)
{
    return a->kind == SLATE_CMD_SET && b->kind == SLATE_CMD_SET && a->id == b->id && a->prop == SLATE_PROP_X &&
           b->prop == SLATE_PROP_Y;
}

/* Every delivered batch, oldest first. */
static void apply_all(slate_engine_t *e)
{
    const slate_cmd_t *c;
    size_t n;
    uint32_t batches = 0, cmds = 0, errors = 0;
    while ((n = slate_cmd_take(e->cmds, &c)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (i + 1 < n && is_move(&c[i], &c[i + 1])) {
                /* x then y of one element (set(x: .., y: ..)): one move, so the box in between is not redrawn */
                slate_el_t *el = map_find(e, c[i].id);
                bool ok_x = false, ok_y = false;
                if (el) {
                    slate_el_move(el, &c[i].v, &c[i + 1].v, dirty_for(e, el), e->w, e->h, &ok_x, &ok_y);
                }
                errors += !ok_x + !ok_y;
                i++;
                continue;
            }
            errors += !apply(e, &c[i]);
        }
        slate_cmd_release(e->cmds);
        batches++;
        cmds += (uint32_t)n;
    }
    if (batches) {
#if SLATE_DEBUG
        slate_debug_cmds(&e->dbg, cmds);
#endif
        publish(e, batches, cmds, errors);
    }
}

/* Images that finished loading since the last look: the IMAGE elements whose image is READY now show it, and their
   boxes are dirty (slate_el_image_sync). The loader's listener kicks the task (images.h). */
static void sync_images(slate_engine_t *e)
{
    uint32_t done = slate_images_done();
    if (done == e->images_seen) {
        return;
    }
    e->images_seen = done;
    for (uint32_t i = 0; i < e->map_cap; i++) {
        slate_el_t *el = e->map[i].el;
        if (el && el->kind == SLATE_EL_IMAGE && el->u.image.im && !el->u.image.sli &&
            slate_image_state(el->u.image.im) == SLATE_IMAGE_READY) {
            slate_el_image_sync(el, dirty_for(e, el), e->w, e->h);
        }
    }
}

/* ---- debug: the statistics line and the FPS counter ---- */

#if SLATE_DEBUG
#define OVL_PAD 2

static const struct display_font_entry *debug_font(void)
{
    for (size_t i = 0; i < display_fonts_count; i++) {
        if (display_fonts[i].name && strcmp(display_fonts[i].name, "default") == 0) {
            return &display_fonts[i];
        }
    }
    return display_fonts_count > 0 ? &display_fonts[0] : NULL;
}

static void overlay_text(slate_engine_t *e, int fps, slate_dirty_t *d)
{
    char txt[16];
    snprintf(txt, sizeof txt, "%d fps", fps);
    slate_value_t v = slate_v_text(txt);
    slate_el_set(e->ovl_text, SLATE_PROP_TEXT, &v, d, e->w, e->h);
    const slate_text_t *t = &e->ovl_text->u.text;
    int tw = t->bx1 - t->bx0, th = t->by1 - t->by0;
    v = slate_v_num((float)(OVL_PAD - t->bx0));
    slate_el_set(e->ovl_text, SLATE_PROP_X, &v, d, e->w, e->h);
    v = slate_v_num((float)(OVL_PAD - t->by0));
    slate_el_set(e->ovl_text, SLATE_PROP_Y, &v, d, e->w, e->h);
    v = slate_v_num((float)(tw + 2 * OVL_PAD));
    slate_el_set(e->ovl_bg, SLATE_PROP_W, &v, d, e->w, e->h);
    v = slate_v_num((float)(th + 2 * OVL_PAD));
    slate_el_set(e->ovl_bg, SLATE_PROP_H, &v, d, e->w, e->h);
    v = slate_v_num((float)((e->w - (tw + 2 * OVL_PAD)) / 2));   /* centred: inside the circle of a round panel */
    slate_el_set(e->ovl, SLATE_PROP_X, &v, d, e->w, e->h);
    e->ovl_fps = fps;
}

/* The FPS counter: a group centred horizontally at y = h / 8 (inside the visible circle of a round panel), on top
   of the scene (a translucent rounded box and the text in the
   default font). No font: no counter. */
static void overlay_new(slate_engine_t *e)
{
    const struct display_font_entry *font = debug_font();
    if (!font) {
        return;
    }
    e->ovl = slate_el_new(SLATE_EL_GROUP);
    e->ovl_bg = slate_el_new(SLATE_EL_ROUNDRECT);
    e->ovl_text = slate_el_new(SLATE_EL_TEXT);
    if (!e->ovl || !e->ovl_bg || !e->ovl_text) {
        slate_el_free(e->ovl);
        slate_el_free(e->ovl_bg);
        slate_el_free(e->ovl_text);
        e->ovl = e->ovl_bg = e->ovl_text = NULL;
        return;
    }
    slate_value_t v = slate_v_num((float)(e->h / 8));
    slate_el_set(e->ovl, SLATE_PROP_Y, &v, NULL, e->w, e->h);   /* x: centred by overlay_text() */
    v = slate_v_num(3);
    slate_el_set(e->ovl_bg, SLATE_PROP_RADIUS, &v, NULL, e->w, e->h);
    v = slate_v_paint((slate_paint_t){ 0xA0000000u, NULL });
    slate_el_set(e->ovl_bg, SLATE_PROP_FILL, &v, NULL, e->w, e->h);
    v = slate_v_font(font);
    slate_el_set(e->ovl_text, SLATE_PROP_FONT, &v, NULL, e->w, e->h);
    v = slate_v_paint((slate_paint_t){ 0xFFFFFFFFu, NULL });
    slate_el_set(e->ovl_text, SLATE_PROP_COLOR, &v, NULL, e->w, e->h);
    slate_el_add(e->ovl, e->ovl_bg, NULL, e->w, e->h);
    slate_el_add(e->ovl, e->ovl_text, NULL, e->w, e->h);
    overlay_text(e, 0, NULL);
    slate_el_add(e->screen, e->ovl, NULL, e->w, e->h);   /* drawn with the first scene shown */
}

/* Once a second: the statistics line (also for a second without frames: "0.0 fps", a still screen), printed by
   debug_print(), and the counter (which dirties only its own box when its value changes). */
static void debug_tick(slate_engine_t *e)
{
    float fps;
    if (!slate_debug_tick(&e->dbg, now_us(), e->map_live, (uint32_t)e->w * (uint32_t)e->h, &fps, e->dbg_line,
                          sizeof e->dbg_line)) {
        return;
    }
    int f = (int)(fps + 0.5f);
    if (e->ovl && f != e->ovl_fps) {
        bool scene = e->scene_dirty;
        overlay_text(e, f, shown(e, e->ovl) && e->scene ? &e->dirty : NULL);
        e->scene_dirty = scene;   /* the counter's own box is not a scene change */
    }
}

/* The lines kept by debug_tick() and the frame, once the frame (if any) has handed over its last band: the console
   is slow and blocks, so it stays out of the frames and of their timings. */
static void debug_print(slate_engine_t *e)
{
    char over[96];
    bool any = e->dbg_line[0] != '\0';
    if (any) {
        printf("%s\n", e->dbg_line);
        e->dbg_line[0] = '\0';
    }
    if (slate_debug_over(&e->dbg, over, sizeof over)) {
        printf("%s\n", over);
        any = true;
    }
    if (any) {
        fflush(stdout);
    }
}
#endif

/* ---- a frame ---- */

/* The dirty set is small enough to race the scan (beam.h). */
static bool races(const slate_engine_t *e)
{
    uint64_t area = 0;
    for (int i = 0; i < e->dirty.n; i++) {
        area += (uint64_t)(e->dirty.r[i].x1 - e->dirty.r[i].x0) * (uint64_t)(e->dirty.r[i].y1 - e->dirty.r[i].y0);
    }
    return e->has_te && e->scan != DISPLAY_SCAN_UNKNOWN
           && area * 100 <= (uint64_t)e->w * (uint64_t)e->h * SLATE_BEAM_AREA_MAX_PCT;
}

static void frame(slate_engine_t *e)
{
    bool scene = e->scene_dirty;
    int rects = e->dirty.n;
    int64_t t0 = now_us();
    uint32_t period;
    e->race = races(e) && slate_beam_model(&e->te, t0, &e->frame_te_us, &period);
    e->wait_acc_us = 0;
    e->frame_ok = e->frame_refused = 0;
    slate_stats_t st;
    slate_compose(e->screen, &e->dirty, &e->target, emit, e, &st);
    scratch_trim(e, st.scratch_want, now_us());
    if (e->held_n > 0) {
        release_held(e, e->held_n - 1);   /* the frame ends with its last band handed over */
    }
    int64_t t1 = now_us();
    e->last_frame_us = t0;
    if (e->frame_ok > 0) {
        e->refused_frames = 0;
    } else if (e->frame_refused > 0 && ++e->refused_frames >= SLATE_REFUSED_FRAMES) {
        refused(e, e->last_err);   /* a panel that refuses everything, whatever the code */
    }
    if (st.aborted) {
        bm_hal_lock_enter(e->lock);
        e->st.aborted++;
        e->st.last = st;
        bm_hal_lock_exit(e->lock);
        return;   /* the dirty set stays: retried at the next pulse */
    }
    e->scene_dirty = false;
    e->drawn = true;
    if (e->retry.n > 0) {
        /* bands the panel refused: sent again by the next frames, a few times in a row at most */
        if (++e->retry_frames <= RETRY_FRAMES) {
            for (int i = 0; i < e->retry.n; i++) {
                slate_dirty_add(&e->dirty, e->retry.r[i], e->w, e->h);
            }
            e->scene_dirty = true;
        }
        slate_dirty_clear(&e->retry);
    } else {
        e->retry_frames = 0;
    }
    uint32_t frame_us = (uint32_t)(t1 - t0), wait_us = e->wait_acc_us > frame_us ? frame_us : e->wait_acc_us;
    if (scene) {
        /* fps: the intervals between scene frames, smoothed; a pause of a second or more starts again */
        int64_t dt = t0 - e->prev_scene_us;
        if (dt > 0 && dt < SECOND_US) {
            float inst = 1e6f / (float)dt;
            e->fps = e->fps > 0 ? e->fps + (inst - e->fps) * 0.2f : inst;
        } else {
            e->fps = 0;
        }
        e->prev_scene_us = t0;
#if SLATE_DEBUG
        slate_debug_frame(&e->dbg, frame_us, frame_us - wait_us, wait_us, rects, &st, e->period_us, e->scene_name);
#endif
    }
    (void)rects;
    bm_hal_lock_enter(e->lock);
    e->st.frames++;
    e->st.frame_us = frame_us;
    e->st.wait_us = wait_us;
    e->st.compose_us = frame_us - wait_us;
    e->st.last = st;
    e->st.scratch_bytes = e->scratch_bytes;
    e->st.push_errors = e->push_errors;
    e->st.beam_waits = e->beam_waits;
    e->st.cache_bytes = e->cache_bytes;
    e->st.fps = e->fps;
    if (scene) {
        e->last_scene_us = t1;
    }
    bm_hal_lock_exit(e->lock);
}

/* How long the task may sleep (ms, UINT32_MAX = until a message). */
static uint32_t sleep_ms(slate_engine_t *e)
{
    int64_t until = INT64_MAX;
    if (e->dirty.n > 0 && !failed(e) && !load(&e->paused)) {
        until = e->has_te ? e->dirty_since_us + (int64_t)SLATE_TE_FALLBACK_MS * 1000
                          : e->last_frame_us + e->period_us;
    }
#if SLATE_DEBUG
    if (slate_debug_due(&e->dbg) < until) {
        until = slate_debug_due(&e->dbg);
    }
#endif
    if (e->scratch && e->scratch_full_us + SECOND_US < until) {
        until = e->scratch_full_us + SECOND_US;   /* scratch_trim */
    }
    if (until == INT64_MAX) {
        return UINT32_MAX;
    }
    int64_t d = until - now_us();
    return d <= 0 ? 0 : (uint32_t)((d + 999) / 1000);
}

/* One wake-up of the task, after the commands: a frame when one is due. */
static void step(slate_engine_t *e, bool pulse)
{
    if (e->dirty.n == 0 || failed(e) || load(&e->paused)) {
        /* nothing to draw, the panel stopped answering or sleeps: no frame (the commands are still applied) */
        store(&e->want_vsync, 0);
        swap(&e->vsync_flag, 0);   /* a pulse that came meanwhile does not start the next frame */
        return;
    }
    if (e->has_te) {
        if (!load(&e->want_vsync)) {
            store(&e->want_vsync, 1);   /* from now on the pulses wake the task */
            e->dirty_since_us = now_us();
        }
        bool late = now_us() - e->dirty_since_us >= (int64_t)SLATE_TE_FALLBACK_MS * 1000;
        if (pulse || late) {
            if (!pulse) {
                /* no pulse for a long while (the TE pin taken by another object, a panel asleep): frame anyway */
                bm_hal_lock_enter(e->lock);
                e->st.te_fallbacks++;
                bm_hal_lock_exit(e->lock);
            }
            frame(e);
            e->dirty_since_us = now_us();   /* still dirty (retry, abort): wait for the next pulse again */
        }
    } else if (now_us() - e->last_frame_us >= e->period_us) {
        frame(e);
    }
}

static void run(slate_engine_t *e)
{
    for (;;) {
        uint32_t m, ms = sleep_ms(e);
        if (ms > 0) {
            bm_hal_queue_receive(e->wake, &m, ms);
        }
        if (load(&e->stop_flag)) {
            return;
        }
        swap(&e->kick_flag, 0);
        bool pulse = swap(&e->vsync_flag, 0) != 0;
        apply_all(e);
        sync_images(e);
        if (swap(&e->redraw_flag, 0) && e->drawn) {
            dirty_all(e);   /* the panel lost its picture: the whole screen again */
        }
#if SLATE_DEBUG
        debug_tick(e);
#endif
        step(e, pulse);
        scratch_trim(e, SIZE_MAX, now_us());
#if SLATE_DEBUG
        debug_print(e);
#endif
    }
}

/* ---- the task ---- */

static void *ext_scratch(size_t n)
{
    return bm_hal_alloc(n, BM_HAL_MEM_INTERNAL);
}

/* The scratch a band of rows rows wants: 3 bytes a pixel of it (compose.h). */
static size_t scratch_want(const slate_engine_t *e, int rows)
{
    return (size_t)3 * (size_t)e->w * (size_t)rows;
}

/* The scratch on demand (compose.h, scratch_grow): n bytes when internal RAM has a block for them, else the
   largest of n / 2, n / 4, ... above the scratch there is (the old one is freed first, and taken again when none
   fits). A size
   that was refused is not asked for again within a second (the compositor asks before every group it composes
   offscreen). */
static void *scratch_grow(void *ctx, size_t n, size_t *got)
{
    slate_engine_t *e = ctx;
    int64_t now = now_us();
    bool refused = e->scratch_refused && n >= e->scratch_refused && now - e->scratch_refused_us < SECOND_US;
    if (n > e->scratch_bytes && !refused) {
        /* the old one first: nothing lives in it, and the new one may fit only with its room */
        size_t had = e->scratch_bytes;
        bm_hal_free(e->scratch);
        e->scratch = NULL;
        e->scratch_bytes = 0;
        for (size_t m = n; m > had; m /= 2) {
            e->scratch = ext_scratch(m);
            if (e->scratch) {
                e->scratch_bytes = m;
                break;
            }
        }
        if (!e->scratch && had) {
            e->scratch = ext_scratch(had);   /* in its own place again, as a rule */
            e->scratch_bytes = e->scratch ? had : 0;
        }
        e->scratch_full_us = now;
        if (e->scratch_bytes < n) {
            e->scratch_refused = n;
            e->scratch_refused_us = now;
        }
    }
    e->target.scratch = e->scratch;
    e->target.scratch_bytes = e->scratch_bytes;
    *got = e->scratch_bytes;
    return e->scratch;
}

/* Gives the scratch back (a new scene: it asks again for what it needs). Between frames only. */
static void scratch_drop(slate_engine_t *e)
{
    bm_hal_free(e->scratch);
    e->scratch = NULL;
    e->scratch_bytes = 0;
    e->scratch_refused = 0;
    e->scratch_need = 0;
    e->target.scratch = NULL;
    e->target.scratch_bytes = 0;
    bm_hal_lock_enter(e->lock);
    e->st.scratch_bytes = 0;
    bm_hal_lock_exit(e->lock);
}

/* After a frame that asked for want bytes of scratch (compose.h, scratch_want), and when the task wakes: a scratch
   that no frame asked for in full for a second shrinks to the most they asked for meanwhile (none: freed). So a
   cache built once, a group shown for a while, give their internal RAM back to the rest of the program. Between
   frames only. */
static void scratch_trim(slate_engine_t *e, size_t want, int64_t now)
{
    if (!e->scratch) {
        return;
    }
    if (want == SIZE_MAX) {
        want = 0;   /* a wake-up without a frame */
    }
    if (want >= e->scratch_bytes) {
        e->scratch_full_us = now;   /* a new second starts */
        e->scratch_need = 0;
        return;
    }
    e->scratch_need = want > e->scratch_need ? want : e->scratch_need;
    if (now - e->scratch_full_us < SECOND_US) {
        return;
    }
    size_t n = e->scratch_need;
    bm_hal_free(e->scratch);   /* first: the smaller one may fit only in its place */
    e->scratch = n ? ext_scratch(n) : NULL;
    e->scratch_bytes = e->scratch ? n : 0;
    e->scratch_need = 0;
    e->scratch_full_us = now;
    e->target.scratch = e->scratch;
    e->target.scratch_bytes = e->scratch_bytes;
    bm_hal_lock_enter(e->lock);
    e->st.scratch_bytes = e->scratch_bytes;
    bm_hal_lock_exit(e->lock);
}

/* Band buffers (the driver's), id table and screen root; false on any failure (what was had is freed by teardown).
   The scratch is not taken here: the compositor asks for it when a group needs it (scratch_grow). */
static bool setup(slate_engine_t *e)
{
    const display_band_ops_t *ops = e->ops;
    int rows = ops->rows ? ops->rows : MIN_ROWS;
    int max_rows = (int)(ops->bytes_max / ((uint32_t)e->w * 2));
    rows = rows > e->h ? e->h : rows;
    rows = rows > max_rows ? max_rows : rows;
    if (rows < 1) {
        return false;
    }
    /* A block of the whole scratch is held while the bands are taken, then given back: the bands then go where the
       free memory is in smaller pieces, and one piece stays large enough for the scratch when a group asks for it
       (the heap's regions do not join, and a region is filled before the next is tried). */
    void *hold = ext_scratch(scratch_want(e, rows));
    for (;;) {
        size_t bytes = (size_t)rows * (size_t)e->w * 2;
        e->buf[0] = ops->buf_alloc(e->panel, bytes);
        e->buf[1] = e->buf[0] ? ops->buf_alloc(e->panel, bytes) : NULL;
        if (e->buf[0] && e->buf[1] && !((uintptr_t)e->buf[0] & 3) && !((uintptr_t)e->buf[1] & 3)) {
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (e->buf[i]) {
                ops->buf_free(e->panel, e->buf[i]);
                e->buf[i] = NULL;
            }
        }
        if (hold) {   /* the bands come first: the same rows again without it */
            bm_hal_free(hold);
            hold = NULL;
            continue;
        }
        if (rows <= MIN_ROWS) {
            return false;
        }
        rows = rows / 2 < MIN_ROWS ? MIN_ROWS : rows / 2;
    }
    bm_hal_free(hold);
    e->rows = rows;
    e->map = ext_alloc(MAP_MIN * sizeof *e->map);
    if (!e->map) {
        return false;
    }
    memset(e->map, 0, MAP_MIN * sizeof *e->map);
    e->map_cap = MAP_MIN;
    e->screen = slate_el_new(SLATE_EL_GROUP);
    if (!e->screen) {
        return false;
    }
    e->target = (slate_target_t){
        .w = e->w, .h = e->h, .rows = rows, .ctx = e,
        .band_buf = band_buf, .wait_free = wait_free, .cache_alloc = cache_alloc, .cache_free = cache_free,
        .scratch = e->scratch, .scratch_bytes = e->scratch_bytes, .background = 0,
        .scratch_grow = scratch_grow, .scratch_max = scratch_want(e, rows),
    };
    slate_compose_underlay(true);
#if SLATE_DEBUG
    overlay_new(e);
    slate_debug_init(&e->dbg, now_us());
#endif
    return true;
}

/* Waits for the bands in flight, frees the scene, the compositor's memory, the band buffers and scratch. */
static void teardown(slate_engine_t *e)
{
    for (int i = 0; i < 2; i++) {
        wait_free(e, i);   /* bounded: a buffer whose done does not come is lost, not freed */
    }
    /* Detach every element first: a group frees its children with it, and they have their own ids. */
    for (uint32_t i = 0; i < e->map_cap; i++) {
        slate_el_t *el = e->map[i].el;
        if (el && el->parent) {
            slate_el_remove(el, NULL, e->w, e->h);
        }
    }
    for (uint32_t i = 0; i < e->map_cap; i++) {
        if (e->map[i].el) {
            slate_el_free(e->map[i].el);
        }
    }
    ext_free(e->map);
    e->map = NULL;
    e->map_cap = e->map_live = e->map_used = 0;
    slate_el_free(e->screen);   /* and the FPS counter in it */
    e->screen = e->scene = NULL;
    slate_compose_release();
    for (int i = 0; i < 2; i++) {
        if (e->buf[i] && !e->lost[i]) {   /* a lost buffer may still be read by the driver: leaked on purpose */
            e->ops->buf_free(e->panel, e->buf[i]);
            e->buf[i] = NULL;
        }
    }
    scratch_drop(e);
}

static void engine_main(void *arg)
{
    slate_engine_t *e = arg;
    bm_hal_queue_t *ctl = e->ctl;
    if (!setup(e)) {
        teardown(e);
        send_msg(ctl, CTL_FAILED, UINT32_MAX);
        return;   /* the PHP task frees e: not a byte of it is touched any more */
    }
    bm_hal_lock_enter(e->lock);
    e->st.band_rows = e->rows;
    e->st.scratch_bytes = e->scratch_bytes;
    bm_hal_lock_exit(e->lock);
    send_msg(ctl, CTL_READY, UINT32_MAX);
    run(e);
    teardown(e);
    send_msg(ctl, CTL_JOINED, UINT32_MAX);
}

/* ---- PHP task ---- */

static void release(slate_engine_t *e)
{
    slate_cmdbuf_free(e->cmds);
    slate_queue_put(e->wake);
    slate_queue_put(e->freed);
    slate_queue_put(e->ctl);
    slate_lock_put(e->lock);
    bm_hal_free(e);
}

slate_engine_t *slate_engine_start(const display_band_ops_t *ops, void *panel, int w, int h, float refresh_hz,
                                   bool has_te)
{
    if (s_running || !ops || !ops->buf_alloc || !ops->buf_free || !ops->begin || !ops->push || !ops->end
        || w <= 0 || h <= 0 || w > 32767 || h > 32767) {
        return NULL;
    }
    /* Internal RAM: the TE interrupt and the driver's done use its flags (atomics do not work in PSRAM on every
       chip). */
    slate_engine_t *e = bm_hal_alloc(sizeof *e, BM_HAL_MEM_INTERNAL);
    if (!e) {
        return NULL;
    }
    memset(e, 0, sizeof *e);
    e->ops = ops;
    e->panel = panel;
    e->w = w;
    e->h = h;
    e->has_te = has_te;
    e->scan = has_te ? ops->scan : DISPLAY_SCAN_UNKNOWN;
    slate_xfer_init(&e->xfer);
    e->period_us = refresh_hz > 0 ? (uint32_t)(1e6f / refresh_hz + 0.5f) : 16000;
    e->slot[0] = (band_slot_t){ .e = e, .i = 0 };
    e->slot[1] = (band_slot_t){ .e = e, .i = 1 };
    e->wake = slate_queue_get();
    e->freed = slate_queue_get();
    e->ctl = slate_queue_get();
    e->lock = slate_lock_get();
    e->cmds = slate_cmdbuf_new();
    if (!e->wake || !e->freed || !e->ctl || !e->lock || !e->cmds) {
        release(e);
        return NULL;
    }
    e->cache_budget = probe_budget();
    e->st.cache_budget = e->cache_budget;
    e->last_frame_us = now_us() - e->period_us;
    e->prev_scene_us = INT64_MIN / 2;
    e->last_scene_us = INT64_MIN / 2;
    slate_scene_set_alloc(ext_alloc, ext_free);
    if (!bm_hal_task_start(engine_main, e, "slate", SLATE_ENGINE_STACK, SLATE_ENGINE_PRIORITY,
                           SLATE_ENGINE_CORE)) {
        release(e);
        return NULL;
    }
    uint32_t m = 0;
    while (m != CTL_READY && m != CTL_FAILED) {
        bm_hal_queue_receive(e->ctl, &m, UINT32_MAX);   /* anything else is a stale message of a recycled queue */
    }
    if (m != CTL_READY) {
        release(e);
        return NULL;
    }
    if (e->scan != DISPLAY_SCAN_UNKNOWN) {
        if (!s_timer) {
            s_timer = bm_hal_timer_create(timer_cb, NULL, "slate");   /* none: the waits end on the queue's clock */
        }
        __atomic_store_n(&s_timer_q, e->freed, __ATOMIC_RELEASE);
    }
    s_running = e;
    return e;
}

bool slate_engine_stop(slate_engine_t *e)
{
    if (!e) {
        return true;
    }
    store(&e->stop_flag, 1);
    send_msg(e->wake, MSG_STOP, 0);   /* a full queue wakes the task as well */
    int64_t deadline = now_us() + (int64_t)SLATE_STOP_TIMEOUT_MS * 1000;
    uint32_t m = 0;
    while (m != CTL_JOINED) {
        int64_t left = deadline - now_us();
        if (left <= 0) {
            /* The task is stuck inside a band op of the driver: it still uses e, its queues and the compositor.
               Everything is left as it is (leaked) and no other engine may start. */
            fail(e, ERR_TIMEOUT);
            return false;
        }
        bm_hal_queue_receive(e->ctl, &m, (uint32_t)((left + 999) / 1000));
    }
    s_running = NULL;
    __atomic_store_n(&s_timer_q, NULL, __ATOMIC_RELEASE);
    if (s_timer) {
        bm_hal_timer_stop(s_timer);   /* a send it already began lands in the recycled queue: harmless */
    }
    if (e->lost[0] || e->lost[1]) {
        /* A late done may still write e->busy and send to e->freed: e and its queues stay allocated (leaked). */
        slate_cmdbuf_free(e->cmds);
        e->cmds = NULL;
        return false;
    }
    release(e);
    return true;
}

const char *slate_engine_error(slate_engine_t *e)
{
    return __atomic_load_n(&e->error, __ATOMIC_SEQ_CST);
}

slate_cmdbuf_t *slate_engine_cmds(slate_engine_t *e)
{
    return e->cmds;
}

void slate_engine_kick(slate_engine_t *e)
{
    if (swap(&e->kick_flag, 1) == 0) {
        send_msg(e->wake, MSG_KICK, 0);
    }
}

bool slate_engine_deliver(slate_engine_t *e)
{
    if (!slate_cmd_deliver(e->cmds)) {
        return false;
    }
    slate_engine_kick(e);
    return true;
}

void slate_engine_vsync(void *engine)
{
    slate_engine_t *e = engine;
    slate_beam_pulse(&e->te, now_us());   /* every pulse, dirty or not: the scan's period and phase */
    if (!load(&e->want_vsync)) {
        return;   /* nothing dirty: the task sleeps on */
    }
    if (swap(&e->vsync_flag, 1) == 0) {
        uint32_t m = MSG_VSYNC;
        bm_hal_queue_send_isr(e->wake, &m);
    }
}

void slate_engine_pause(slate_engine_t *e, bool asleep)
{
    store(&e->paused, asleep ? 1 : 0);
    slate_engine_kick(e);   /* awake: the changes made meanwhile go */
}

void slate_engine_redraw(slate_engine_t *e)
{
    store(&e->redraw_flag, 1);
    slate_engine_kick(e);
}

void slate_engine_stats(slate_engine_t *e, slate_engine_stats_t *out)
{
    bm_hal_lock_enter(e->lock);
    *out = e->st;
    int64_t last = e->last_scene_us;
    bm_hal_lock_exit(e->lock);
    if (now_us() - last > SECOND_US) {
        out->fps = 0;
    }
    out->cache_budget = __atomic_load_n(&e->cache_budget, __ATOMIC_RELAXED);
}

void slate_engine_set_cache_budget(slate_engine_t *e, size_t bytes)
{
    __atomic_store_n(&e->cache_budget, bytes, __ATOMIC_RELAXED);
}
