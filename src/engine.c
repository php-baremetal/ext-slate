/* Slate's compositor task: see engine.h. */
#include <math.h>
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
#include "tween.h"

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
#define TW_DONE_MIN 16         /* ids in the first block of the done queue */
#define TW_DONE_MAX 4096       /* ids waiting at most: more are lost (counted) */
#ifndef SLATE_RECLAIM_WAIT_MS
#define SLATE_RECLAIM_WAIT_MS 100      /* the image loader's longest wait for the engine to drop the kept caches */
#endif
#ifndef SLATE_RECLAIM_STOP_MS
#define SLATE_RECLAIM_STOP_MS (SLATE_RECLAIM_WAIT_MS + 1000)   /* slate_engine_stop's wait for a reclaim under way */
#endif
#define ERR_RECLAIM "an image load waiting for memory did not end in time"
#define KEEP_LIMITS 1                  /* keep_flag bits: the limits changed */
#define KEEP_RECLAIM 2                 /*   the image loader asks for the kept caches */
#define TR_DONE_MAX 64         /* transition dones waiting at most: more are lost (counted) */
#define SC_HOLD_US 500000      /* the bars stay this long after a scroll stops ... */
#define SC_FADE_US 200000      /* ... then fade to 0 in this */
#define SC_IDLE_TICK_US 50000  /* while the panel sleeps: a moving scroll is stepped this often (no frames) */
#define HIT_RETRY_US 100000    /* a hit table refused memory: tried again this much later */
#define SLATE_STR(x) SLATE_STR2(x)
#define SLATE_STR2(x) #x

typedef struct { uint32_t id; slate_el_t *el; } map_ent_t;
/* A tween of the scene (engine task only). */
typedef struct tw {
    struct tw *next;               /* the engine's list, in the order the tweens came */
    slate_tween_t *t;
    slate_el_t *el;
    uint32_t id;                   /* the PHP task's (0: an ease's, nobody waits for it) */
    bool dead;                     /* done or stopped: freed by tw_sweep() */
    bool hidden;                   /* paused: its element is not shown */
    bool removed;                  /* its element had a REMOVE in the batches being applied */
    bool scroll;                   /* a scrollTo: it animates SCROLL_X / SCROLL_Y of a scroll */
    bool sc_done;                  /* a scrollTo done in the tick in progress: its scroll end after the tween dones */
} tw_t;
/* A scroll the engine drives (engine task only): made by its first SCROLL command (CONFIG, as a rule), gone with its
   element (FREE) or when the group stops being a scroll. Its clock is the ticks' time minus off: it stands still while
   the scroll is not shown (as a tween's). */
typedef struct sc {
    struct sc *next;
    slate_el_t *el;
    uint32_t id;                   /* the element's id (the PHP task's) */
    slate_scroll_pub_t *pub;       /* where its position goes (a reference held), NULL: nowhere */
    uint32_t tween;                /* the scrollTo that runs (its tween id), 0: none */
    int64_t off;                   /* its clock: the ticks' time - off */
    int64_t hid_at;                /* the ticks' time it was hidden at */
    int64_t still;                 /* its clock when it stopped moving (the bars' fade) */
    float ind;                     /* the bars' alpha written */
    bool ind_on;                   /* bars (CONFIG indicators) */
    bool moving;                   /* as published */
    bool hidden;                   /* not shown: its clock stands still */
    bool removed;                  /* its element had a REMOVE in the batches being applied */
} sc_t;
typedef struct { uint32_t id; int16_t x, y; } sce_t;   /* a scroll end */
/* The ease settings of an element (engine task only). */
typedef struct ez { struct ez *next; slate_el_t *el; uint32_t mask; int32_t ms; uint8_t ease; } ez_t;
typedef struct { uint32_t id; int64_t end; } fin_t;   /* a tween done in the tick in progress */
typedef struct { uint32_t from, to; } trd_t;          /* a transition done: the roots' ids */
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
    int tr_flag;                   /* a transition runs (slate_engine_in_transition) */
    int keep_flag;                 /* KEEP_LIMITS: the limits changed (slate_engine_set_keep); KEEP_RECLAIM: the image
                                      loader asks for the kept caches (reclaim()) */
    bm_hal_queue_t *ack;           /* the engine answers a reclaim with the last request number it served (even), bit 0
                                      set when it freed something */
    uint32_t reclaim_req;          /* atomic: the last request number (the loader's; steps of 2) */
    int hit_flag;                  /* slate_engine_set_hit_pub() gave a block (hit_new, under lock) */
    bool reclaim_stuck;            /* stop: a reclaim did not end in time; e and its queues are left allocated */
    int keep_scenes_req;           /* the limits asked for */
    size_t keep_budget_req;
    int busy[2];                   /* band buffer i handed to push, its done still to come */
    slate_te_t te;                 /* the TE pulses: written by the interrupt */
    size_t cache_budget;
    const char *error;             /* sticky: the engine stopped composing (slate_engine_error) */
    char error_text[64];           /* the error with a code (refused()): written once, before error points at it */

    /* under lock */
    slate_engine_stats_t st;
    int64_t last_scene_us;
    uint32_t *dq;                  /* the done queue: ids of tweens ended by themselves, a ring (grown by the engine) */
    uint32_t dq_cap, dq_head, dq_n;
    trd_t trq[TR_DONE_MAX];        /* the transition done queue, a ring */
    uint32_t trq_head, trq_n;
    sce_t *scq;                    /* the scroll end queue, a ring (grown by the engine, as dq) */
    uint32_t scq_cap, scq_head, scq_n;
    slate_hit_pub_t *hit_new;      /* the block slate_engine_set_hit_pub() gave (a reference), not taken yet */

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
    uint32_t scene_id;             /* the id of the last SHOW (0: none) */
    uint32_t bg_argb;              /* its background */
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
    tw_t *tws, *tws_tail;          /* the tweens */
    ez_t *ezs;                     /* the ease settings */
    uint32_t tw_live;              /* tweens not dead */
    int tw_run;                    /* tweens that need frames (running, shown) */
    bool tw_check;                 /* the scene's shape changed: tw_sync() after the batches */
    int64_t tw_last_us;            /* the time of the last tick (ticks never go back) */
    fin_t *fin;                    /* the tweens done in the tick in progress */
    uint32_t fin_n, fin_cap;
    bool fin_woke;                 /* a done went to the queue directly (no room in fin) and found it empty */
    void (*tw_cb)(void *arg);      /* the tween listener (copied at start); also told of transition dones */
    void *tw_arg;
    bool apply_pulse;              /* the batches being applied came with a pulse (their time: the pulse's) */
    struct {
        bool on;
        slate_pair_t p;
        int64_t t0;                /* its start (the ticks' time base) */
        int64_t dur;               /* us */
        uint8_t ease;
        uint32_t from_id, to_id;
        bool keep;                 /* the scene it leaves keeps its caches at its end (its SHOW's keep) */
    } tr;                          /* the transition that runs */
    slate_el_t **kept;             /* the roots of the scenes left that keep their caches, oldest first */
    uint32_t kept_n, kept_cap;
    int keep_scenes;               /* the limits (engine.h, Kept scenes) */
    size_t keep_budget;            /* 0: none of its own */
    size_t kept_bytes;             /* their cache memory, counted when the list changes */
    uint32_t kept_dropped;
    uint32_t reclaims;             /* reclaim requests served */
    sc_t *scs;                     /* the scrolls driven */
    int sc_run;                    /* scrolls that need ticks (shown, moving or bars fading) */
    int64_t sc_wake;               /* the end of the earliest bars' hold (ticks' time; INT64_MAX: none): a timed wake */
    slate_hit_pub_t *hit;          /* where the hit table goes (a reference), NULL: nowhere (engine.h, Touch) */
    slate_hit_ids_t hit_ids;       /* the ids of the touchable elements */
    bool hit_due;                  /* the table is to build again (after the frame that comes, if one comes) */
    int64_t hit_retry_us;          /* after a build refused memory: the next try not before this (0: none due) */
    int hit_n;                     /* entries of the table published */
#if SLATE_DEBUG
    slate_debug_t dbg;
    slate_el_t *ovl, *ovl_bg, *ovl_text;
    int ovl_fps;
    char dbg_line[200];            /* the statistics line still to print ("" none) */
    uint8_t dbg_nocache;           /* 1: a cached scroll was composed directly, its line still to print; 2: printed
                                      (once per engine) */
    uint8_t dbg_hitnomem;          /* 1: a hit table was refused memory, its line still to print; 2: printed (once
                                      per engine) */
#endif
};

static slate_engine_t *s_running;   /* PHP task only */
static void (*s_tw_cb)(void *arg);  /* slate_engine_set_tween_listener(): read by slate_engine_start */
static void *s_tw_arg;
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

static bool kept_evict(slate_engine_t *e);
static void kept_recount(slate_engine_t *e);

/* Element caches: external RAM, refused past the budget. Each block carries its size in front (8 bytes, so the
   block stays 8-byte aligned). The caches of the scenes left go first, oldest first, when the block does not fit
   (in the budget, or in external RAM) and would fit without them (engine.h, Kept scenes). */
static void *cache_alloc(void *ctx, size_t n)
{
    slate_engine_t *e = ctx;
    size_t budget = __atomic_load_n(&e->cache_budget, __ATOMIC_RELAXED);
    if (n > budget) {
        return NULL;
    }
    if (e->cache_bytes > budget - n) {
        kept_recount(e);   /* a write to a kept scene may have dropped caches since the list changed */
        size_t kept = e->kept_bytes < e->cache_bytes ? e->kept_bytes : e->cache_bytes;
        if (e->cache_bytes - kept > budget - n) {
            return NULL;   /* would not fit with every kept cache gone: they stay */
        }
        while (e->cache_bytes > budget - n) {
            if (!kept_evict(e)) {
                return NULL;
            }
        }
    }
    uint8_t *p;
    while (!(p = bm_hal_alloc(n + CACHE_HDR, BM_HAL_MEM_EXTERNAL_ONLY))) {   /* never internal RAM */
        if (!kept_evict(e)) {
            return NULL;
        }
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

static bool probe_fits(size_t n)
{
    void *p = bm_hal_alloc(n, BM_HAL_MEM_EXTERNAL_ONLY);
    bm_hal_free(p);
    return p != NULL;
}

/* Default cache budget: half of the largest external block found, at most SLATE_CACHE_BUDGET_MAX; 0 without
   external RAM (the probe never takes internal RAM). The largest power of two that fits (from
   2 * SLATE_CACHE_BUDGET_MAX down), then the sixteenths above it: within 1/16 of the block, not the power of two
   below it (1 008 KB free gave 256 KB, short of a 360 x 360 underlay). */
static size_t probe_budget(void)
{
    for (size_t n = 2 * (size_t)SLATE_CACHE_BUDGET_MAX; n >= 16384; n /= 2) {
        if (probe_fits(n)) {
            if (n < 2 * (size_t)SLATE_CACHE_BUDGET_MAX) {
                for (size_t k = 15; k > 0; k--) {
                    size_t size = n + n / 16 * k;
                    if (probe_fits(size)) {
                        return size / 2;
                    }
                }
            }
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

/* ---- the hit table (engine.h, Touch) ---- */

/* A change of el (a command): when touchables of the scene shown are in its subtree, the table is due again (the
   frame the change gives, if any, builds it; a change that draws nothing, such as TOUCHABLE or HIT_PAD, too). */
static void hit_touch(slate_engine_t *e, const slate_el_t *el)
{
    if (e->hit && slate_el_touchables(el) > 0 && shown(e, el)) {
        e->hit_due = true;
        e->hit_retry_us = 0;   /* a new cause: no wait after a refusal */
    }
}

static void hit_stats(slate_engine_t *e, bool built, bool nomem)
{
    bm_hal_lock_enter(e->lock);
    e->st.hit_builds += built;
    e->st.hit_nomem += nomem;
    e->st.hit_entries = (uint32_t)e->hit_n;
    bm_hal_lock_exit(e->lock);
}

/* An empty table (SHOW: until the new scene's first frame). */
static void hit_clear(slate_engine_t *e)
{
    if (e->hit) {
        slate_hit_pub_publish(e->hit, 0);
        e->hit_n = 0;
        hit_stats(e, false, false);
        e->hit_due = true;
    }
}

/* At the end of a wake-up: the table built again when due, unless a transition runs (touches are ignored meanwhile)
   or a frame is still to come (the table follows the frame composed). Zero touchables in the root shown: no walk
   (an empty table published once). */
static void hit_sync(slate_engine_t *e)
{
    if (load(&e->hit_flag)) {
        bm_hal_lock_enter(e->lock);
        slate_hit_pub_t *p = e->hit_new;
        e->hit_new = NULL;
        store(&e->hit_flag, 0);
        bm_hal_lock_exit(e->lock);
        slate_hit_pub_put(e->hit);
        e->hit = p;
        e->hit_n = 0;
        e->hit_due = p != NULL;
        e->hit_retry_us = 0;
        if (p) {
            slate_hit_pub_publish(p, 0);   /* a block given again starts empty: never another scene's entries */
            hit_stats(e, false, false);
        }
    }
    if (e->scene_dirty && e->dirty.n == 0) {
        /* the scene shown changed without a pixel (rows added outside a scroll's view, a scroll's limits or axis):
           no frame comes, the table follows now */
        e->scene_dirty = false;
        e->hit_due |= e->hit != NULL;
    }
    if (!e->hit_due || !e->hit || e->tr.on || (e->dirty.n > 0 && !failed(e) && !load(&e->paused))) {
        return;
    }
    if (e->hit_retry_us && now_us() < e->hit_retry_us) {
        return;   /* refused memory a moment ago: the timed wake-up (sleep_ms) tries again */
    }
    e->hit_due = false;
    e->hit_retry_us = 0;
    if (!e->scene || slate_el_touchables(e->scene) == 0) {
        if (e->hit_n > 0) {
            slate_hit_pub_publish(e->hit, 0);
            e->hit_n = 0;
            hit_stats(e, false, false);
        }
        return;
    }
    int n = slate_hit_build(e->hit, e->scene, e->w, e->h, &e->hit_ids);
    if (n < 0) {
#if SLATE_DEBUG
        if (e->dbg_hitnomem == 0) {
            e->dbg_hitnomem = 1;   /* printed by debug_print(), once */
        }
#endif
        hit_stats(e, false, true);
        e->hit_due = true;   /* tried again HIT_RETRY_US later, even on a still screen */
        e->hit_retry_us = now_us() + HIT_RETRY_US;
        return;
    }
    e->hit_n = n;
    hit_stats(e, true, false);
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
static void tw_forget(slate_engine_t *e, slate_el_t *el);
static void tw_sync(slate_engine_t *e, int64_t now);
static void sc_sync(slate_engine_t *e, int64_t now);
static void sc_limits(slate_engine_t *e, sc_t *sc);
static sc_t *sc_of(slate_engine_t *e, const slate_el_t *el);
static sc_t *sc_new(slate_engine_t *e, slate_el_t *el, uint32_t id);
static int64_t sc_clock(const sc_t *sc, int64_t t);
static void sc_update(slate_engine_t *e, sc_t *sc, int64_t t);
static void sc_count(slate_engine_t *e, int64_t now);
static void sc_drop(slate_engine_t *e, sc_t *sc);
static void sc_tween_start(slate_engine_t *e, tw_t *en, int64_t now);
static int64_t pulse_us(const slate_engine_t *e);

/* The time of the batches being applied, on the ticks' time base (as tw_sync's): the pulse that came with them, else
   now; never before the last tick. */
static int64_t batch_us(slate_engine_t *e)
{
    int64_t t = e->apply_pulse && e->has_te ? pulse_us(e) : now_us();
    return t < e->tw_last_us ? e->tw_last_us : t;
}

/* A transition's done (the roots' ids) into its queue; the listener when the queue was empty. */
static void tr_queue_done(slate_engine_t *e, uint32_t from, uint32_t to)
{
    bool woke = false;
    bm_hal_lock_enter(e->lock);
    if (e->trq_n < TR_DONE_MAX) {
        e->trq[(e->trq_head + e->trq_n) % TR_DONE_MAX] = (trd_t){ from, to };
        woke = e->trq_n++ == 0;
    } else {
        e->st.transition_dones_lost++;
    }
    e->st.transitions++;
    bm_hal_lock_exit(e->lock);
    if (woke && e->tw_cb) {
        e->tw_cb(e->tw_arg);
    }
}

/* ---- kept scenes (engine.h, Kept scenes) ---- */

/* The cache memory under el. */
static size_t tree_bytes(const slate_el_t *el)
{
    size_t n = 0;
    const slate_cache_t *c = el->cache;
    if (c) {
        n += (c->px ? c->cbytes : 0) + (c->sa ? c->sbytes : 0);
    }
    if (el->kind == SLATE_EL_GROUP) {
        for (const slate_el_t *k = el->u.g.first; k; k = k->next) {
            n += tree_bytes(k);
        }
    }
    return n;
}

static void kept_publish(slate_engine_t *e)
{
    bm_hal_lock_enter(e->lock);
    e->st.kept_scenes = e->kept_n;
    e->st.kept_bytes = e->kept_bytes;
    e->st.kept_dropped = e->kept_dropped;
    e->st.reclaims = e->reclaims;
    e->st.cache_bytes = e->cache_bytes;
    bm_hal_lock_exit(e->lock);
}

static int kept_find(const slate_engine_t *e, const slate_el_t *el)
{
    for (uint32_t i = 0; i < e->kept_n; i++) {
        if (e->kept[i] == el) {
            return (int)i;
        }
    }
    return -1;
}

/* The index of the kept root el is in (el itself, or its top), -1 when none. */
static int kept_top(const slate_engine_t *e, const slate_el_t *el)
{
    if (!e->kept_n || !el) {
        return -1;
    }
    while (el->parent) {
        el = el->parent;
    }
    return kept_find(e, el);
}

/* Takes root i off the list (its caches stay with it). */
static void kept_unlink(slate_engine_t *e, int i)
{
    e->kept_n--;
    memmove(&e->kept[i], &e->kept[i + 1], (e->kept_n - (uint32_t)i) * sizeof e->kept[0]);
}

static void kept_recount(slate_engine_t *e)
{
    size_t n = 0;
    for (uint32_t i = 0; i < e->kept_n; i++) {
        n += tree_bytes(e->kept[i]);
    }
    e->kept_bytes = n;
}

/* The oldest kept root drops its caches; false when none is kept. */
static bool kept_evict(slate_engine_t *e)
{
    if (e->kept_n == 0) {
        return false;
    }
    size_t before = e->cache_bytes;
    slate_cache_drop_tree(e->kept[0]);
    size_t freed = before - e->cache_bytes;
    e->kept_bytes = e->kept_bytes > freed ? e->kept_bytes - freed : 0;
    kept_unlink(e, 0);
    if (e->kept_n == 0) {
        e->kept_bytes = 0;
    }
    e->kept_dropped++;
    kept_publish(e);
    return true;
}

/* Within the limits: the oldest go. */
static void kept_trim(slate_engine_t *e)
{
    kept_recount(e);
    while (e->kept_n > 0 &&
           ((int64_t)e->kept_n > e->keep_scenes || (e->keep_budget && e->kept_bytes > e->keep_budget))) {
        kept_evict(e);
    }
    kept_publish(e);
}

/* The limits asked for by slate_engine_set_keep, when they changed. Also called when a scene is left: set_keep
   stores them before the SHOW that follows it is pushed, so a SHOW applied in the same wake-up (the task already past
   the check at its top) sees them. */
static void keep_sync(slate_engine_t *e)
{
    int f = load(&e->keep_flag) ? swap(&e->keep_flag, 0) : 0;
    if (f & KEEP_LIMITS) {
        e->keep_scenes = __atomic_load_n(&e->keep_scenes_req, __ATOMIC_SEQ_CST);
        e->keep_budget = __atomic_load_n(&e->keep_budget_req, __ATOMIC_SEQ_CST);
        kept_trim(e);
    }
    if (f & KEEP_RECLAIM) {   /* an image needs the memory: every kept cache goes, then the loader is told */
        uint32_t req = __atomic_load_n(&e->reclaim_req, __ATOMIC_SEQ_CST);   /* even */
        uint32_t ans = req | (e->kept_n > 0 ? 1u : 0u);   /* bit 0: something was freed */
        while (kept_evict(e)) {
        }
        e->reclaims++;
        kept_publish(e);
        bm_hal_queue_send(e->ack, &ans, 0);
    }
}

/* Scene root el is left: kept (keep, the limits allow one, it has caches), else its caches go (rebuilt when it is
   shown again). */
static void kept_leave(slate_engine_t *e, slate_el_t *el, bool keep)
{
    keep_sync(e);
    if (!el) {
        return;
    }
    if (keep && e->keep_scenes > 0 && tree_bytes(el) > 0) {
        if (e->kept_n == e->kept_cap) {
            uint32_t cap = e->kept_cap ? e->kept_cap * 2 : 4;
            slate_el_t **k = ext_alloc(cap * sizeof *k);
            if (k) {
                if (e->kept_n) {
                    memcpy(k, e->kept, e->kept_n * sizeof *k);
                }
                ext_free(e->kept);
                e->kept = k;
                e->kept_cap = cap;
            }
        }
        if (e->kept_n < e->kept_cap) {
            e->kept[e->kept_n++] = el;
            kept_trim(e);
            return;
        }
    }
    slate_cache_drop_tree(el);
}

/* The kept root el (if it is one) is shown, or goes into a group: off the list, its caches with it. */
static void kept_take(slate_engine_t *e, const slate_el_t *el)
{
    int i = e->kept_n ? kept_find(e, el) : -1;
    if (i >= 0) {
        kept_unlink(e, i);
        kept_recount(e);
        kept_publish(e);
    }
}

/* The transition that runs ends: the new scene alone under the screen on its background, the scene left loses its
   caches (its tweens pause at the next tw_sync: tw_check), the underlay comes back, the whole screen is dirty, the
   done is queued (the listener when the queue was empty). The scene left keeps its caches when its SHOW said so
   (Kept scenes). */
static void tr_finish(slate_engine_t *e)
{
    slate_el_t *from = e->tr.p.from;
    slate_pair_end(&e->tr.p);
    e->tr.on = false;
    store(&e->tr_flag, 0);
#if SLATE_DEBUG
    overlay_top(e);
#endif
    kept_leave(e, from, e->tr.keep);   /* kept, or its caches go */
    scratch_drop(e);
    slate_compose_underlay(true);
    e->target.background = rgb565(e->bg_argb);
    e->tw_check |= e->tws != NULL;
    dirty_all(e);
    tr_queue_done(e, e->tr.from_id, e->tr.to_id);
}

/* The transition at now: the layers where its eased time puts them (the whole screen dirty when they moved); or its
   end. */
static void tr_tick(slate_engine_t *e, int64_t now)
{
    int64_t t = now - e->tr.t0;
    if (t >= e->tr.dur) {
        tr_finish(e);
        if (e->tw_check) {
            e->tw_check = false;
            tw_sync(e, now);   /* the tweens of the scene left pause now */
        }
        if (e->scs) {
            sc_sync(e, now);   /* and its scrolls */
        }
        return;
    }
    float x = t > 0 ? (float)t / (float)e->tr.dur : 0.0f;
    if (slate_pair_at(&e->tr.p, slate_ease((slate_ease_t)e->tr.ease, x))) {
        dirty_all(e);   /* the same pixel, the same opacity: no frame for it */
    }
}

static bool apply_show(slate_engine_t *e, const slate_cmd_t *c)
{
    slate_el_t *s = c->id ? map_find(e, c->id) : NULL;
    if ((c->id && (!s || s->kind != SLATE_EL_GROUP)) || !slate_trans_ok(&c->tr)) {
        return false;
    }
    if (s && s->parent && s != e->scene && !(e->tr.on && s == e->tr.p.from)) {
        return false;      /* a group in another one (refused before a transition is ended for it) */
    }
    uint32_t bg = c->v.type == SLATE_VAL_COLOR ? c->v.v.argb : 0;
    if (e->tr.on) {
        if (s == e->scene) {
            /* the scene it goes to: it goes on; only the name and the background change (the background where that
               scene shows it: its curtain, or the target's under an UNCOVER) */
            snprintf(e->scene_name, sizeof e->scene_name, "%s", c->data ? (const char *)c->data : "");
            if (bg != e->bg_argb) {
                e->bg_argb = bg;
                if (e->tr.p.kind == SLATE_TRANS_UNCOVER) {
                    e->target.background = rgb565(bg);
                    e->tr.p.under_bg = bg;
                } else {
                    slate_value_t f = slate_v_paint((slate_paint_t){ bg | 0xFF000000u, NULL });
                    slate_el_set(e->tr.p.curtain, SLATE_PROP_FILL, &f, NULL, e->w, e->h);
                }
                dirty_all(e);
            }
            return true;
        }
        tr_finish(e);      /* it jumps to its end (its done), then this SHOW */
    }
    bool trans = false, cut = false;
    uint32_t from_id = e->scene_id;
    if (e->scene != s) {
        slate_el_t *old = e->scene;
        if (s) {
            kept_take(e, s);   /* a kept scene comes back with its caches */
        }
        if (s && c->tr.kind != SLATE_TRANS_NONE &&
            slate_pair_begin(&e->tr.p, e->screen, old, s, (slate_trans_kind_t)c->tr.kind, (slate_dir_t)c->tr.dir,
                             e->bg_argb, bg, e->w, e->h)) {
            /* both scenes under the screen until the end (their tweens run); no underlay meanwhile */
            trans = true;
            slate_compose_underlay(false);
            slate_compose_underlay_free();
            e->tr.on = true;
            e->tr.t0 = batch_us(e);
            e->tr.dur = (int64_t)c->tr.ms * 1000;
            e->tr.ease = c->tr.ease;
            e->tr.from_id = e->scene_id;
            e->tr.to_id = c->id;
            e->tr.keep = c->keep;
            store(&e->tr_flag, 1);
            e->target.background = rgb565(e->tr.p.under_bg);
        } else {
            cut = s && c->tr.kind != SLATE_TRANS_NONE;   /* the transition could not begin (memory): a cut */
            if (old) {
                slate_el_remove(old, NULL, e->w, e->h);
                kept_leave(e, old, c->keep);   /* kept, or its caches go: rebuilt when it is shown again */
                scratch_drop(e);   /* the new scene asks for what it needs */
            }
            if (s) {
                slate_el_add(e->screen, s, NULL, e->w, e->h);
            }
        }
        e->scene = s;
#if SLATE_DEBUG
        overlay_top(e);
#endif
    }
    e->tw_check |= e->tws != NULL;   /* the tweens of the scene that went pause, those of the new one resume */
    snprintf(e->scene_name, sizeof e->scene_name, "%s", c->data ? (const char *)c->data : "");
    e->scene_id = c->id;
    e->bg_argb = bg;
    if (!trans) {
        e->target.background = rgb565(bg);
    }
    dirty_all(e);
    if (cut) {
        tr_queue_done(e, from_id, c->id);   /* a cut still ends with its done */
    }
    return true;
}

static bool apply_free(slate_engine_t *e, uint32_t id)
{
    slate_el_t *el = map_find(e, id);
    if (!el) {
        return false;
    }
    if (e->tr.on && (el == e->tr.p.from || el == e->tr.p.to)) {
        tr_finish(e);   /* a scene of the transition goes: it ends first (its done) */
    }
    hit_touch(e, el);
    if (el == e->scene) {
        slate_el_remove(el, NULL, e->w, e->h);
        e->scene = NULL;
        dirty_all(e);
    }
    int ki = kept_top(e, el);
    if (ki >= 0) {   /* in a kept scene: the caches under it go first (its children leave without them) */
        slate_cache_drop_tree(el);
        if (e->kept[ki] == el) {
            kept_unlink(e, ki);
        }
        kept_recount(e);
        kept_publish(e);
    }
    if (el->kind == SLATE_EL_GROUP) {
        while (el->u.g.first) {   /* the children have their own ids: they stay */
            slate_el_t *ch = el->u.g.first;
            slate_el_remove(ch, dirty_for(e, ch), e->w, e->h);
        }
    }
    slate_el_remove(el, dirty_for(e, el), e->w, e->h);
    sc_t *sc = e->scs ? sc_of(e, el) : NULL;
    if (sc) {
        sc_drop(e, sc);   /* no end; its children's scrolls pause (sc_sync) */
        sc_count(e, batch_us(e));
    }
    if (el->anim || e->tws) {
        tw_forget(e, el);   /* its tweens stop, those of its children pause (tw_sync) */
    }
    if (el->touchable) {
        slate_hit_ids_del(&e->hit_ids, el);
    }
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

/* ---- tweens ---- */

static void sc_tween_end(slate_engine_t *e, tw_t *en, bool done, int64_t now);

static tw_t *tw_find(slate_engine_t *e, uint32_t id)
{
    for (tw_t *en = e->tws; en && id; en = en->next) {
        if (!en->dead && en->id == id) {
            return en;
        }
    }
    return NULL;
}

static tw_t *tw_of(slate_engine_t *e, const slate_tween_t *t)
{
    for (tw_t *en = e->tws; en; en = en->next) {
        if (!en->dead && en->t == t) {
            return en;
        }
    }
    return NULL;
}

static ez_t *ez_find(slate_engine_t *e, const slate_el_t *el)
{
    for (ez_t *z = e->ezs; z; z = z->next) {
        if (z->el == el) {
            return z;
        }
    }
    return NULL;
}

/* A new tween of el, at the end of the list; NULL when out of memory (t is not freed). */
static tw_t *tw_add(slate_engine_t *e, slate_tween_t *t, slate_el_t *el, uint32_t id)
{
    tw_t *en = ext_alloc(sizeof *en);
    if (!en) {
        return NULL;
    }
    *en = (tw_t){ .t = t, .el = el, .id = id };
    if (e->tws_tail) {
        e->tws_tail->next = en;
    } else {
        e->tws = en;
    }
    e->tws_tail = en;
    e->tw_live++;
    el->anim = true;
    e->tw_check = true;   /* hidden or not: tw_sync() */
    return en;
}

/* The from values of en: el's values now (a chained tween that starts, or that stop(to_end) ends unstarted). */
static void tw_from(tw_t *en)
{
    slate_tween_prop_t *p = slate_tween_props(en->t);
    for (int i = 0; i < slate_tween_count(en->t); i++) {
        slate_value_t v;
        if (slate_el_anim_get(en->el, p[i].prop, &v) && v.type == p[i].type) {
            if (v.type == SLATE_VAL_COLOR) {
                p[i].from.argb = v.v.argb;
            } else {
                p[i].from.num = v.v.num;
            }
        }
    }
}

static void tw_kill(slate_engine_t *e, tw_t *en, bool to_end);

/* prop of el is no longer animated by the tweens that run (started): a write, or a tween that takes it over. An
   endless tween left without properties is stopped (no done: it never ends; nothing would need its frames); a finite
   one runs on to its done. The caller sweeps (tw_sync, tw_tick). */
static void tw_drop_prop(slate_engine_t *e, const slate_el_t *el, slate_prop_t prop, const tw_t *except)
{
    for (tw_t *o = e->tws; o; o = o->next) {
        if (o != except && !o->dead && o->el == el && slate_tween_started(o->t) && slate_tween_drop_prop(o->t, prop)
            && slate_tween_count(o->t) == 0 && slate_tween_endless(o->t)) {
            tw_kill(e, o, false);
            e->tw_check = true;
        }
    }
}

/* en starts: it takes its properties over from the other tweens of its element. */
static void tw_take_over(slate_engine_t *e, tw_t *en)
{
    const slate_tween_prop_t *p = slate_tween_props(en->t);
    for (int i = 0; i < slate_tween_count(en->t); i++) {
        tw_drop_prop(e, en->el, p[i].prop, en);
    }
}

/* Stops en and the tweens chained after it, without a done; to_end: each writes its final values first. */
static void tw_kill(slate_engine_t *e, tw_t *en, bool to_end)
{
    for (tw_t *o = e->tws; o; o = o->next) {
        if (!o->dead && slate_tween_next(o->t) == en->t) {
            slate_tween_set_next(o->t, NULL);   /* its predecessor does not start it any more */
        }
    }
    for (tw_t *c = en; c && !c->dead;) {
        if (to_end) {
            slate_tween_prop_t v[SLATE_TWEEN_PROPS_MAX];
            int n;
            if (!slate_tween_started(c->t)) {
                tw_from(c);
            }
            slate_tween_final(c->t, v, &n);
            slate_tween_write(c->el, v, n, dirty_for(e, c->el), e->w, e->h);
        }
        c->dead = true;
        e->tw_live--;
        if (c->scroll) {
            sc_tween_end(e, c, false, batch_us(e));   /* a scrollTo stopped: no end */
        }
        slate_tween_t *nx = slate_tween_next(c->t);
        c = nx ? tw_of(e, nx) : NULL;
    }
}

/* el still has tweens or ease settings (its anim flag). */
static void anim_recheck(slate_engine_t *e, slate_el_t *el)
{
    for (tw_t *o = e->tws; o; o = o->next) {
        if (!o->dead && o->el == el) {
            return;
        }
    }
    el->anim = ez_find(e, el) != NULL;
}

/* Frees the dead tweens; gone: an element being freed (its flag is not touched). */
static void tw_sweep(slate_engine_t *e, const slate_el_t *gone)
{
    tw_t **pp = &e->tws, *prev = NULL;
    while (*pp) {
        tw_t *en = *pp;
        if (!en->dead) {
            prev = en;
            pp = &en->next;
            continue;
        }
        *pp = en->next;
        slate_el_t *el = en->el;
        slate_tween_free(en->t);
        ext_free(en);
        if (el != gone) {
            anim_recheck(e, el);
        }
    }
    e->tws_tail = prev;
}

/* Counts the tweens that need frames, and publishes the tweens alive. */
static void tw_count(slate_engine_t *e)
{
    int run = 0;
    for (tw_t *en = e->tws; en; en = en->next) {
        run += !en->dead && !en->hidden && slate_tween_active(en->t);
    }
    e->tw_run = run;
    bm_hal_lock_enter(e->lock);
    e->st.tweens_active = e->tw_live;
    bm_hal_lock_exit(e->lock);
}

/* After the batches that changed the scene's shape: a tween whose element had a REMOVE and has no parent now stops
   (a REMOVE and an ADD in one batch move it); the tweens of elements no longer shown pause, those shown again
   resume, at now. */
static void tw_sync(slate_engine_t *e, int64_t now)
{
    if (now < e->tw_last_us) {
        now = e->tw_last_us;
    }
    e->tw_last_us = now;
    for (tw_t *en = e->tws; en; en = en->next) {
        if (!en->dead && en->removed) {
            en->removed = false;
            if (!en->el->parent && en->el != e->scene) {
                tw_kill(e, en, false);
            }
        }
    }
    for (tw_t *en = e->tws; en; en = en->next) {
        if (en->dead) {
            continue;
        }
        bool vis = shown(e, en->el);
        if (!vis && !en->hidden) {
            slate_tween_pause(en->t, now);
            en->hidden = true;
        } else if (vis && en->hidden) {
            slate_tween_resume(en->t, now);
            en->hidden = false;
        }
    }
    tw_sweep(e, NULL);
    tw_count(e);
}

/* The tweens of an element that is freed stop (and those chained after them), its ease settings go. */
static void tw_forget(slate_engine_t *e, slate_el_t *el)
{
    for (tw_t *en = e->tws; en; en = en->next) {
        if (!en->dead && en->el == el) {
            tw_kill(e, en, false);
        }
    }
    for (ez_t **pz = &e->ezs; *pz; pz = &(*pz)->next) {
        if ((*pz)->el == el) {
            ez_t *z = *pz;
            *pz = z->next;
            ext_free(z);
            break;
        }
    }
    tw_sweep(e, el);
    e->tw_check = true;
}

/* id into the done queue (PHP task: slate_engine_tween_done); true when the queue was empty. */
static bool dq_push(slate_engine_t *e, uint32_t id)
{
    uint32_t *old = NULL;
    bool was_empty = false;
    bm_hal_lock_enter(e->lock);
    if (e->dq_n == e->dq_cap && e->dq_cap < TW_DONE_MAX) {
        /* only this task changes dq and dq_cap: the new block is taken outside the lock */
        uint32_t cap = e->dq_cap ? e->dq_cap * 2 : TW_DONE_MIN;
        bm_hal_lock_exit(e->lock);
        uint32_t *q = ext_alloc(cap * sizeof *q);
        bm_hal_lock_enter(e->lock);
        if (q) {
            for (uint32_t i = 0; i < e->dq_n; i++) {
                q[i] = e->dq[(e->dq_head + i) % e->dq_cap];
            }
            old = e->dq;
            e->dq = q;
            e->dq_cap = cap;
            e->dq_head = 0;
        }
    }
    if (e->dq_n < e->dq_cap) {
        e->dq[(e->dq_head + e->dq_n) % e->dq_cap] = id;
        was_empty = e->dq_n++ == 0;
    } else {
        e->st.tween_dones_lost++;
    }
    bm_hal_lock_exit(e->lock);
    ext_free(old);
    return was_empty;
}

static void fin_add(slate_engine_t *e, uint32_t id, int64_t end)
{
    if (e->fin_n == e->fin_cap) {
        uint32_t cap = e->fin_cap ? e->fin_cap * 2 : 16;
        fin_t *f = ext_alloc(cap * sizeof *f);
        if (!f) {
            e->fin_woke |= dq_push(e, id);   /* no room to sort it: queued at once, the listener at the flush */
            return;
        }
        if (e->fin_n) {
            memcpy(f, e->fin, e->fin_n * sizeof *f);
        }
        ext_free(e->fin);
        e->fin = f;
        e->fin_cap = cap;
    }
    e->fin[e->fin_n++] = (fin_t){ id, end };
}

/* The tweens done in this tick, in the order they ended (their end times; ties in the order they came), into the
   done queue; the listener when the queue was empty. */
static void fin_flush(slate_engine_t *e)
{
    bool woke = e->fin_woke;
    e->fin_woke = false;
    for (uint32_t i = 1; i < e->fin_n; i++) {
        fin_t f = e->fin[i];
        uint32_t j = i;
        while (j > 0 && e->fin[j - 1].end > f.end) {
            e->fin[j] = e->fin[j - 1];
            j--;
        }
        e->fin[j] = f;
    }
    for (uint32_t i = 0; i < e->fin_n; i++) {
        woke |= dq_push(e, e->fin[i].id);
    }
    e->fin_n = 0;
    if (woke && e->tw_cb) {
        e->tw_cb(e->tw_arg);
    }
}

/* Every running tween at now (the frame's time): the values written into the elements (dirty, caches); a tween
   done is told (with an id), and the tween chained after it starts at its end, from the values it left, and is
   sampled in the same tick (and so on down the chain). */
static void tw_tick(slate_engine_t *e, int64_t now)
{
    if (now < e->tw_last_us) {
        now = e->tw_last_us;
    }
    e->tw_last_us = now;
    bool wrote = false;
    for (tw_t *en = e->tws; en; en = en->next) {
        for (tw_t *c = en; c && !c->dead && !c->hidden;) {
            slate_tween_prop_t v[SLATE_TWEEN_PROPS_MAX];
            int n;
            bool done = slate_tween_sample(c->t, now, v, &n);
            if (n > 0) {
                slate_tween_write(c->el, v, n, dirty_for(e, c->el), e->w, e->h);
                wrote = true;
            }
            if (!done) {
                break;
            }
            c->dead = true;
            e->tw_live--;
            if (c->id) {
                fin_add(e, c->id, slate_tween_end_us(c->t));
            }
            c->sc_done = c->scroll;   /* a scrollTo ended by itself: its scroll end after its done (below) */
            slate_tween_t *nx = slate_tween_next(c->t);
            c = nx ? tw_of(e, nx) : NULL;
            if (c) {
                tw_from(c);
                tw_take_over(e, c);
                if (c->scroll) {
                    sc_tween_start(e, c, now);   /* a chained scrollTo: clamped to the limits it starts with */
                }
            }
        }
    }
    fin_flush(e);
    for (tw_t *en = e->tws; en; en = en->next) {
        if (en->sc_done) {
            en->sc_done = false;
            sc_tween_end(e, en, true, now);   /* after the tween dones: TweenDone comes before ScrollEnded */
        }
    }
    if (wrote) {
        /* a tween of a geometry (the scroll's w / h, a child's box) moves the limits with no delivery (sc_sync) */
        for (sc_t *sc = e->scs; sc; sc = sc->next) {
            sc_limits(e, sc);
        }
    }
    tw_sweep(e, NULL);
    tw_count(e);
}

/* The earliest end of a running tween (INT64_MAX: none, or only endless ones). */
static int64_t tw_next_end(slate_engine_t *e)
{
    int64_t end = INT64_MAX;
    for (tw_t *en = e->tws; en; en = en->next) {
        if (!en->dead && !en->hidden && slate_tween_active(en->t)) {
            int64_t t = slate_tween_end_us(en->t);
            end = t < end ? t : end;
        }
    }
    return end;
}

/* The earliest end of an animation (a running tween, the transition), INT64_MAX: none. */
static int64_t anim_next_end(slate_engine_t *e)
{
    int64_t end = e->tw_run > 0 ? tw_next_end(e) : INT64_MAX;
    if (e->tr.on && e->tr.t0 + e->tr.dur < end) {
        end = e->tr.t0 + e->tr.dur;
    }
    if (e->sc_run > 0 && e->tw_last_us + SC_IDLE_TICK_US < end) {
        end = e->tw_last_us + SC_IDLE_TICK_US;   /* a scroll's motion has no known end: stepped now and then */
    }
    if (e->sc_wake < end) {
        end = e->sc_wake;   /* the bars' hold ends: their fade needs ticks from then */
    }
    return end;
}

static void sc_tick(slate_engine_t *e, int64_t now);

/* The animations at now (the frame's time): the tweens, then the transition. */
static void anim_tick(slate_engine_t *e, int64_t now)
{
    __atomic_add_fetch(&e->st.anim_ticks, 1, __ATOMIC_RELAXED);   /* no lock per tick: stats() copies it under its lock */
    if (e->tw_run > 0) {
        tw_tick(e, now);
    }
    if (e->sc_run > 0) {
        sc_tick(e, now);   /* after the tweens (a scrollTo's values are written), before the frame */
    }
    if (e->tr.on) {
        if (now < e->tw_last_us) {
            now = e->tw_last_us;
        }
        e->tw_last_us = now;
        tr_tick(e, now);
    }
}

static bool apply_tween(slate_engine_t *e, const slate_cmd_t *c)
{
    const slate_tween_spec_t *s = c->data;
    slate_el_t *el = map_find(e, c->id);
    if (!el || !s || s->tween == 0 || s->n < 1 || s->n > SLATE_TWEEN_PROPS_MAX || tw_find(e, s->tween)) {
        return false;
    }
    slate_tween_prop_t p[SLATE_TWEEN_PROPS_MAX];
    for (int i = 0; i < s->n; i++) {
        slate_value_t v;
        p[i] = s->props[i];
        for (int j = 0; j < i; j++) {
            if (p[j].prop == p[i].prop) {
                return false;   /* a property twice */
            }
        }
        if (!slate_el_anim_get(el, p[i].prop, &v) || v.type != p[i].type ||
            (v.type == SLATE_VAL_NUM && !isfinite(p[i].to.num))) {
            return false;
        }
        if (v.type == SLATE_VAL_COLOR) {
            p[i].from.argb = v.v.argb;
        } else {
            p[i].from.num = v.v.num;
        }
    }
    /* a scrollTo: a tween of a scroll's SCROLL_X / SCROLL_Y (its targets clamped when it starts: sc_tween_start) */
    bool scroll_to = false;
    for (int i = 0; slate_el_scroll(el) && i < s->n; i++) {
        scroll_to |= p[i].prop == SLATE_PROP_SCROLL_X || p[i].prop == SLATE_PROP_SCROLL_Y;
    }
    sc_t *sc = scroll_to ? sc_of(e, el) : NULL;
    if (scroll_to && !sc && !(sc = sc_new(e, el, c->id))) {
        return false;
    }
    tw_t *pred = s->after ? tw_find(e, s->after) : NULL;
    slate_tween_t *t = pred ? slate_tween_new_chained(p, s->n, s->ms, (slate_ease_t)s->ease, s->delay_ms, s->repeat,
                                                      s->yoyo)
                            : slate_tween_new(p, s->n, batch_us(e), s->ms, (slate_ease_t)s->ease, s->delay_ms,
                                              s->repeat, s->yoyo);
    if (!t) {
        return false;
    }
    if (pred && !slate_tween_set_next(pred->t, t)) {
        slate_tween_free(t);
        return false;
    }
    tw_t *en = tw_add(e, t, el, s->tween);
    if (!en) {
        for (slate_tween_t *q = pred ? pred->t : NULL; q; q = slate_tween_next(q)) {
            if (slate_tween_next(q) == t) {
                slate_tween_set_next(q, NULL);
                break;
            }
        }
        slate_tween_free(t);
        return false;
    }
    if (!pred) {
        tw_take_over(e, en);
    }
    if (sc) {
        en->scroll = true;
        if (!pred) {
            sc_tween_start(e, en, batch_us(e));   /* a chained one: when it starts (tw_tick) */
        }
    }
    return true;
}

static bool apply_ease(slate_engine_t *e, const slate_cmd_t *c)
{
    const slate_ease_spec_t *s = c->data;
    slate_el_t *el = map_find(e, c->id);
    if (!el || !s) {
        return false;
    }
    ez_t **pz = &e->ezs;
    while (*pz && (*pz)->el != el) {
        pz = &(*pz)->next;
    }
    if (s->mask == 0) {
        if (*pz) {
            ez_t *z = *pz;
            *pz = z->next;
            ext_free(z);
            anim_recheck(e, el);
        }
        return true;
    }
    if (s->ms < 1 || s->ease >= SLATE_EASE_COUNT) {
        return false;
    }
    if (!*pz) {
        ez_t *z = ext_alloc(sizeof *z);
        if (!z) {
            return false;
        }
        *z = (ez_t){ .el = el };
        *pz = z;
    }
    (*pz)->mask = s->mask;
    (*pz)->ms = s->ms;
    (*pz)->ease = s->ease;
    el->anim = true;
    return true;
}

/* A SET's value as a tween's target (NUM, or a solid colour); false for any other value. */
static bool anim_target(const slate_cmd_t *c, slate_tween_prop_t *p)
{
    *p = (slate_tween_prop_t){ .prop = c->prop };
    if (c->v.type == SLATE_VAL_NUM && isfinite(c->v.v.num)) {
        p->type = SLATE_VAL_NUM;
        p->to.num = c->v.v.num;
    } else if (c->v.type == SLATE_VAL_COLOR || (c->v.type == SLATE_VAL_PAINT && !c->v.v.paint.grad)) {
        p->type = SLATE_VAL_COLOR;
        p->to.argb = c->v.type == SLATE_VAL_COLOR ? c->v.v.argb : c->v.v.paint.argb;
    } else {
        return false;
    }
    return true;
}

/* A SET of an element with tweens or ease settings: with ease on the property (and the element shown), a tween
   from where it is to the value (none when one already goes there); else the tweens that run stop animating it, and
   the value is set. */
static bool apply_set_anim(slate_engine_t *e, slate_el_t *el, const slate_cmd_t *c)
{
    ez_t *z = ez_find(e, el);
    slate_tween_prop_t p;
    slate_value_t cur;
    if (z && c->prop < 32 && (z->mask & (1u << c->prop)) && shown(e, el) && anim_target(c, &p) &&
        slate_el_anim_get(el, c->prop, &cur) && cur.type == p.type) {
        bool num = p.type == SLATE_VAL_NUM;
        for (tw_t *o = e->tws; o; o = o->next) {
            /* only an ease's own tween (one run, ends at its `to`) makes the write redundant */
            if (o->dead || o->id != 0 || o->el != el || !slate_tween_started(o->t)) {
                continue;
            }
            const slate_tween_prop_t *q = slate_tween_props(o->t);
            for (int i = 0; i < slate_tween_count(o->t); i++) {
                if (q[i].prop == p.prop && (num ? q[i].to.num == p.to.num : q[i].to.argb == p.to.argb)) {
                    return true;   /* already on its way there */
                }
            }
        }
        if (num ? cur.v.num != p.to.num : cur.v.argb != p.to.argb) {
            if (num) {
                p.from.num = cur.v.num;
            } else {
                p.from.argb = cur.v.argb;
            }
            slate_tween_t *t = slate_tween_new(&p, 1, batch_us(e), z->ms, (slate_ease_t)z->ease, 0, 0, false);
            tw_t *en = t ? tw_add(e, t, el, 0) : NULL;
            if (en) {
                tw_take_over(e, en);
                return true;
            }
            slate_tween_free(t);   /* out of memory: the value jumps */
        }
    }
    tw_drop_prop(e, el, c->prop, NULL);
    return slate_el_set(el, c->prop, &c->v, dirty_for(e, el), e->w, e->h);
}

/* ---- scrolls (engine.h, Scrolls) ---- */

static sc_t *sc_of(slate_engine_t *e, const slate_el_t *el)
{
    for (sc_t *sc = e->scs; sc; sc = sc->next) {
        if (sc->el == el) {
            return sc;
        }
    }
    return NULL;
}

static sc_t *sc_new(slate_engine_t *e, slate_el_t *el, uint32_t id)
{
    sc_t *sc = ext_alloc(sizeof *sc);
    if (!sc) {
        return NULL;
    }
    *sc = (sc_t){ .next = e->scs, .el = el, .id = id, .still = INT64_MIN / 2 };
    e->scs = sc;
    return sc;
}

static void sc_drop(slate_engine_t *e, sc_t *sc)
{
    for (sc_t **pp = &e->scs; *pp; pp = &(*pp)->next) {
        if (*pp == sc) {
            *pp = sc->next;
            break;
        }
    }
    slate_scroll_pub_put(sc->pub);
    ext_free(sc);
}

/* The scroll's clock at the ticks' time t (standing still while hidden). */
static int64_t sc_clock(const sc_t *sc, int64_t t)
{
    return (sc->hidden ? sc->hid_at : t) - sc->off;
}

/* An inertia, a snap or a spring: stepped at every tick. */
static bool sc_motion(const slate_scroll_t *ph)
{
    slate_sc_state_t st = slate_scroll_state(ph);
    return st == SLATE_SC_FLING || st == SLATE_SC_SNAP || st == SLATE_SC_SPRING;
}

static int16_t i16(int v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* The drawn position, rounded, and the moving flag into the pub. */
static void sc_publish(const sc_t *sc)
{
    if (!sc->pub) {
        return;
    }
    int dx, dy;
    slate_el_scroll_px(sc->el, &dx, &dy);
    uint32_t w = (uint32_t)(uint16_t)i16(dx) | (uint32_t)(uint16_t)i16(dy) << 16;
    __atomic_store_n(&sc->pub->pos, w, __ATOMIC_SEQ_CST);
    __atomic_store_n(&sc->pub->moving, sc->moving ? 1 : 0, __ATOMIC_SEQ_CST);
}

/* The physics' position into SCROLL_X / SCROLL_Y (dirty as for any change). */
static void sc_write(slate_engine_t *e, sc_t *sc)
{
    float x, y;
    slate_scroll_pos(&sc->el->u.g.scroll->phys, &x, &y);
    slate_dirty_t *d = dirty_for(e, sc->el);
    slate_value_t v = slate_v_num(x);
    slate_el_set(sc->el, SLATE_PROP_SCROLL_X, &v, d, e->w, e->h);
    v = slate_v_num(y);
    slate_el_set(sc->el, SLATE_PROP_SCROLL_Y, &v, d, e->w, e->h);
}

/* The physics stopped where the scroll is drawn (a scrollTo moved only the drawn position). */
static void sc_hold(sc_t *sc)
{
    const slate_scroll_el_t *r = sc->el->u.g.scroll;
    slate_scroll_t *ph = slate_el_scroll(sc->el);
    slate_scroll_jump(ph, r->sx, r->sy);
}

/* A scroll end into its queue (the position drawn); the listener when the queue was empty. */
static void sc_end(slate_engine_t *e, sc_t *sc)
{
    int dx, dy;
    slate_el_scroll_px(sc->el, &dx, &dy);
    sce_t *old = NULL;
    bool woke = false;
    bm_hal_lock_enter(e->lock);
    if (e->scq_n == e->scq_cap && e->scq_cap < TW_DONE_MAX) {
        /* only this task changes scq and scq_cap: the new block is taken outside the lock */
        uint32_t cap = e->scq_cap ? e->scq_cap * 2 : TW_DONE_MIN;
        bm_hal_lock_exit(e->lock);
        sce_t *q = ext_alloc(cap * sizeof *q);
        bm_hal_lock_enter(e->lock);
        if (q) {
            for (uint32_t i = 0; i < e->scq_n; i++) {
                q[i] = e->scq[(e->scq_head + i) % e->scq_cap];
            }
            old = e->scq;
            e->scq = q;
            e->scq_cap = cap;
            e->scq_head = 0;
        }
    }
    if (e->scq_n < e->scq_cap) {
        e->scq[(e->scq_head + e->scq_n) % e->scq_cap] = (sce_t){ sc->id, i16(dx), i16(dy) };
        woke = e->scq_n++ == 0;
    } else {
        e->st.tween_dones_lost++;   /* the tweens' policy and count */
    }
    bm_hal_lock_exit(e->lock);
    ext_free(old);
    if (woke && e->tw_cb) {
        e->tw_cb(e->tw_arg);
    }
}

/* The bars' alpha d us (its clock) after the scroll stopped. */
static float sc_fade(int64_t d)
{
    if (d < SC_HOLD_US) {
        return 1.0f;
    }
    if (d >= SC_HOLD_US + SC_FADE_US) {
        return 0.0f;
    }
    return 1.0f - (float)(d - SC_HOLD_US) / (float)SC_FADE_US;
}

/* After a change, at t (its clock): the moving flag (published), the bars' alpha. */
static void sc_update(slate_engine_t *e, sc_t *sc, int64_t t)
{
    bool mv = slate_scroll_state(&sc->el->u.g.scroll->phys) != SLATE_SC_IDLE || sc->tween != 0;
    if (mv != sc->moving) {
        sc->moving = mv;
        if (!mv) {
            sc->still = t;
        }
    }
    float a = !sc->ind_on ? 0.0f : mv ? 1.0f : sc_fade(t - sc->still);
    if (a != sc->ind) {
        slate_value_t v = slate_v_num(a);
        slate_el_set(sc->el, SLATE_PROP_INDICATOR_A, &v, dirty_for(e, sc->el), e->w, e->h);
        sc->ind = a;
    }
    sc_publish(sc);
}

/* The scroll needs ticks at now (the ticks' time): shown, and moving by itself (inertia, snap, spring, scrollTo) or
   its bars fading (past their hold: during the hold nothing changes, a timed wake-up comes at its end, sc_wake). */
static bool sc_runs(const sc_t *sc, int64_t now)
{
    return !sc->hidden && (sc_motion(&sc->el->u.g.scroll->phys) || sc->tween ||
                           (!sc->moving && sc->ind > 0 && sc_clock(sc, now) - sc->still >= SC_HOLD_US));
}

/* Counts the scrolls that need ticks at now and the earliest end of a bars' hold (sc_wake), and publishes the active
   ones (moving or fading, paused included). */
static void sc_count(slate_engine_t *e, int64_t now)
{
    int run = 0;
    uint32_t live = 0;
    int64_t wake = INT64_MAX;
    for (sc_t *sc = e->scs; sc; sc = sc->next) {
        bool r = sc_runs(sc, now);
        run += r;
        live += sc->moving || sc->ind > 0;
        if (!r && !sc->hidden && !sc->moving && sc->ind > 0) {
            int64_t w = sc->still + SC_HOLD_US + sc->off;   /* its clock back to the ticks' time */
            wake = w < wake ? w : wake;
        }
    }
    e->sc_run = run;
    e->sc_wake = wake;
    bm_hal_lock_enter(e->lock);
    e->st.scrolls_active = live;
    bm_hal_lock_exit(e->lock);
}

/* The scrollTo en of a scroll is gone: done (by itself: a scroll end) or stopped (no end). The physics stays where
   the tween left the drawn position. */
static void sc_tween_end(slate_engine_t *e, tw_t *en, bool done, int64_t now)
{
    sc_t *sc = sc_of(e, en->el);
    if (!sc || sc->tween != en->id || !slate_el_scroll(sc->el)) {
        return;   /* superseded by a newer scrollTo, or not a scroll any more */
    }
    sc->tween = 0;
    sc_hold(sc);
    sc_write(e, sc);   /* the physics clamped it to the limits now (content that shrank under the tween) */
    if (done) {
        sc_end(e, sc);
    }
    sc_update(e, sc, sc_clock(sc, now));
}

/* The SCROLL_X / SCROLL_Y targets of scrollTo en clamped to the scroll's limits now. */
static void sc_tween_clamp(tw_t *en)
{
    slate_scroll_t *ph = slate_el_scroll(en->el);
    if (!ph) {
        return;
    }
    float mx, my;
    slate_scroll_max(ph, &mx, &my);
    slate_tween_prop_t *p = slate_tween_props(en->t);
    for (int i = 0; i < slate_tween_count(en->t); i++) {
        if (p[i].prop == SLATE_PROP_SCROLL_X || p[i].prop == SLATE_PROP_SCROLL_Y) {
            float m = p[i].prop == SLATE_PROP_SCROLL_X ? mx : my;
            p[i].to.num = p[i].to.num < 0.0f ? 0.0f : p[i].to.num > m ? m : p[i].to.num;
        }
    }
}

/* scrollTo en starts (applied, or its predecessor ended): its targets clamped to the limits now; it takes over from the
   physics (a motion stops, no end) and from an earlier scrollTo (which runs on to its done, without a scroll end). */
static void sc_tween_start(slate_engine_t *e, tw_t *en, int64_t now)
{
    sc_t *sc = sc_of(e, en->el);
    slate_scroll_t *ph = slate_el_scroll(en->el);
    if (!sc || !ph) {
        return;
    }
    sc_tween_clamp(en);
    sc->tween = en->id;
    if (slate_scroll_state(ph) != SLATE_SC_IDLE || ph->armed) {
        slate_scroll_jump(ph, en->el->u.g.scroll->sx, en->el->u.g.scroll->sy);
    }
    sc_update(e, sc, sc_clock(sc, now));
    sc_count(e, now);
}

/* Stops the scrollTo of sc (like Tween::stop(): no done, no end). */
static void sc_tween_stop(slate_engine_t *e, sc_t *sc)
{
    uint32_t id = sc->tween;
    if (!id) {
        return;
    }
    sc->tween = 0;
    sc_hold(sc);
    tw_t *en = tw_find(e, id);
    if (en) {
        tw_kill(e, en, false);
        tw_sweep(e, NULL);
        e->tw_check = true;
    }
}

/* One step of sc's motion at t (its clock): the position written, the end queued when the motion ended. */
static void sc_step(slate_engine_t *e, sc_t *sc, int64_t t)
{
    slate_scroll_t *ph = slate_el_scroll(sc->el);   /* the limits brought up to date first */
    slate_scroll_step(ph, t);
    sc_write(e, sc);
    if (slate_scroll_take_end(ph)) {
        sc_end(e, sc);
    }
}

/* Every scroll that needs it at now (the frame's time): a scrollTo's position published, a motion stepped, the bars. */
static void sc_tick(slate_engine_t *e, int64_t now)
{
    if (now < e->tw_last_us) {
        now = e->tw_last_us;
    }
    e->tw_last_us = now;
    for (sc_t *sc = e->scs; sc; sc = sc->next) {
        if (!sc_runs(sc, now)) {
            continue;
        }
        int64_t t = sc_clock(sc, now);
        if (!sc->tween && sc_motion(&sc->el->u.g.scroll->phys)) {
            sc_step(e, sc, t);
        }
        sc_update(e, sc, t);
    }
    sc_count(e, now);
}

/* The limits of sc may have moved (a delivery, or a tween that wrote a geometry: the scroll's w / h, a child's box):
   a scrollTo's targets follow limits that shrank; a scroll at rest is brought into them (drawn and published, no
   end), so that the view shows no space past the edge and the next press anchors where the content is drawn. */
static void sc_limits(slate_engine_t *e, sc_t *sc)
{
    slate_scroll_t *ph = slate_el_scroll(sc->el);   /* the limits brought up to date first */
    if (!ph) {
        return;
    }
    if (sc->tween) {
        tw_t *en = tw_find(e, sc->tween);
        if (en) {
            sc_tween_clamp(en);   /* content that shrank: it ends inside the limits */
        }
    } else if (slate_scroll_state(ph) == SLATE_SC_IDLE && !ph->armed) {
        float x, y;
        slate_scroll_pos(ph, &x, &y);
        if (x != sc->el->u.g.scroll->sx || y != sc->el->u.g.scroll->sy) {
            sc_write(e, sc);
            sc_publish(sc);
        }
    }
}

/* After batches: a scroll whose element had a REMOVE and has no parent now stops (no end) and loses its bars; a drag
   whose Released cannot come any more (the scroll is no longer shown, or a transition runs: the PHP side ignores the
   touch then) ends as a release without velocity (a spring back, a snap, or rest; its end); the scrolls no longer
   shown pause (stepped to now first: they keep the position of that instant), those shown again resume (their clock
   moves on from where it stood); a scrollTo's targets follow limits that shrank; a scroll at rest is brought into
   limits that changed. At now. */
static void sc_sync(slate_engine_t *e, int64_t now)
{
    if (now < e->tw_last_us) {
        now = e->tw_last_us;
    }
    for (sc_t *sc = e->scs; sc; sc = sc->next) {
        slate_scroll_t *ph = slate_el_scroll(sc->el);
        bool vis = shown(e, sc->el);
        if (sc->removed) {
            sc->removed = false;
            if (!sc->el->parent && sc->el != e->scene) {
                sc_tween_stop(e, sc);
                if (slate_scroll_state(ph) != SLATE_SC_IDLE || ph->armed) {
                    sc_hold(sc);   /* stops the motion (and a drag), no end */
                    sc_write(e, sc);
                }
                sc->moving = false;
                sc->still = INT64_MIN / 2;   /* no bars: they would wait for it to come back */
                sc_update(e, sc, sc_clock(sc, now));
            }
        }
        if (ph->armed && (!vis || e->tr.on)) {
            slate_scroll_cancel(ph, sc_clock(sc, now));
            sc_write(e, sc);
            if (slate_scroll_take_end(ph)) {
                sc_end(e, sc);
            }
            sc_update(e, sc, sc_clock(sc, now));
        }
        if (!vis && !sc->hidden) {
            if (!sc->tween && sc_motion(ph)) {
                sc_step(e, sc, sc_clock(sc, now));
            }
            sc_update(e, sc, sc_clock(sc, now));
            sc->hidden = true;
            sc->hid_at = now;
        } else if (vis && sc->hidden) {
            sc->off += now - sc->hid_at;
            sc->hidden = false;
        }
        sc_limits(e, sc);
    }
    sc_count(e, now);
}

/* A SCROLL command (cmd.h, slate_scroll_cmd_t). */
static bool apply_scroll(slate_engine_t *e, const slate_cmd_t *c)
{
    const slate_scroll_cmd_t *s = c->data;
    slate_el_t *el = map_find(e, c->id);
    if (!s || !el || el->kind != SLATE_EL_GROUP) {
        return false;
    }
    sc_t *sc = sc_of(e, el);
    slate_scroll_t *ph = slate_el_scroll(el);
    if (s->op == SLATE_SCROLL_OP_CONFIG) {
        bool reset = false;   /* the position is written only for a new scroll or a new set-up (a scrollTo keeps its) */
        if (!ph) {
            slate_scroll_t init;
            slate_scroll_init(&init, (slate_scroll_axis_t)s->axis, s->bounce, s->snap_x, s->snap_y);
            slate_scroll_snap_max(&init, s->snap_max);
            slate_value_t v = slate_v_scroll(&init);
            if (!slate_el_set(el, SLATE_PROP_SCROLL, &v, dirty_for(e, el), e->w, e->h)) {
                return false;
            }
            ph = slate_el_scroll(el);
            reset = true;
        } else if (ph->axis != (slate_scroll_axis_t)s->axis || ph->bounce != s->bounce ||
                   ph->ax[0].snap != (s->snap_x > 0 ? s->snap_x : 0) ||
                   ph->ax[1].snap != (s->snap_y > 0 ? s->snap_y : 0) ||
                   ph->snap_max != (s->snap_max > 0 ? (s->snap_max > 32767 ? 32767 : s->snap_max) : 0)) {
            /* a new set-up stops what moves (no end) */
            if (sc) {
                sc_tween_stop(e, sc);
            }
            slate_scroll_jump(ph, el->u.g.scroll->sx, el->u.g.scroll->sy);
            ph->axis = (slate_scroll_axis_t)s->axis;
            ph->bounce = s->bounce;
            ph->ax[0].snap = s->snap_x > 0 ? s->snap_x : 0;
            ph->ax[1].snap = s->snap_y > 0 ? s->snap_y : 0;
            slate_scroll_snap_max(ph, s->snap_max);
            float cw, ch;   /* the limits again: a locked axis has none */
            slate_el_scroll_extent(el, &cw, &ch);
            slate_scroll_limits(ph, el->u.g.w > 0 ? el->u.g.w : 0, el->u.g.h > 0 ? el->u.g.h : 0, cw, ch);
            reset = true;
        }
        if (!sc && !(sc = sc_new(e, el, c->id))) {
            return false;
        }
        if (s->pub && s->pub != sc->pub) {
            slate_scroll_pub_ref(s->pub);
            slate_scroll_pub_put(sc->pub);
            sc->pub = s->pub;
        }
        sc->ind_on = s->indicators;
        if (reset) {
            sc_write(e, sc);
            hit_touch(e, el);   /* the axis flags may change without a pixel */
        }
        sc_update(e, sc, sc_clock(sc, batch_us(e)));
        sc_count(e, batch_us(e));
        if (sc->pub) {
            __atomic_store_n(&sc->pub->seq, s->seq, __ATOMIC_SEQ_CST);
        }
        return true;
    }
    if (!ph || (!sc && !(sc = sc_new(e, el, c->id)))) {
        return false;
    }
    int64_t off = sc->off;
    switch ((slate_scroll_op_t)s->op) {
    case SLATE_SCROLL_OP_PRESS:
        sc_tween_stop(e, sc);   /* a scrollTo stops like Tween::stop() */
        slate_scroll_press(ph, s->x, s->y, s->t_us - off);
        break;
    case SLATE_SCROLL_OP_DRAG:
        slate_scroll_drag(ph, s->x, s->y, s->t_us - off);
        sc_write(e, sc);
        break;
    case SLATE_SCROLL_OP_RELEASE:
    case SLATE_SCROLL_OP_CANCEL:
        if (s->op == SLATE_SCROLL_OP_CANCEL) {
            slate_scroll_cancel(ph, s->t_us - off);
        } else {
            slate_scroll_release(ph, s->t_us - off);
        }
        sc_write(e, sc);
        if (slate_scroll_take_end(ph)) {
            sc_end(e, sc);   /* a drag that leaves no motion ends at once */
        }
        break;
    case SLATE_SCROLL_OP_JUMP:
        sc_tween_stop(e, sc);
        slate_scroll_jump(ph, s->x, s->y);
        sc_write(e, sc);
        break;
    default:
        return false;
    }
    sc_update(e, sc, sc_clock(sc, batch_us(e)));
    if (s->op == SLATE_SCROLL_OP_JUMP && sc->pub) {
        __atomic_store_n(&sc->pub->seq, s->seq, __ATOMIC_SEQ_CST);
    }
    sc_count(e, batch_us(e));
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
    case SLATE_CMD_SET: {
        el = map_find(e, c->id);
        bool ok;
        if (el && c->prop == SLATE_PROP_TOUCHABLE && c->v.type == SLATE_VAL_BOOL) {
            /* the id noted first (the walk needs it), forgotten when it is no longer touchable */
            if (c->v.v.b && !slate_hit_ids_put(&e->hit_ids, el, c->id)) {
                return false;   /* out of memory: not touchable */
            }
            hit_touch(e, el);   /* before (it may stop being touchable) and after (it may become touchable) */
            slate_el_set(el, c->prop, &c->v, NULL, e->w, e->h);
            if (!c->v.v.b) {
                slate_hit_ids_del(&e->hit_ids, el);
            }
            hit_touch(e, el);
            return true;
        }
        if (el) {
            hit_touch(e, el);
        }
        if (el && el->anim) {
            ok = apply_set_anim(e, el, c);
        } else {
            ok = el && slate_el_set(el, c->prop, &c->v, dirty_for(e, el), e->w, e->h);
        }
        sc_t *sc;
        if (ok && c->prop == SLATE_PROP_SCROLL && e->scs && (sc = sc_of(e, el)) != NULL) {
            sc->tween = 0;   /* a new physics state (or none): what ran is forgotten */
            if (!slate_el_scroll(el)) {
                sc_drop(e, sc);   /* a plain group again */
            }
            sc_count(e, batch_us(e));
        }
        return ok;
    }
    case SLATE_CMD_ADD:
        g = map_find(e, c->parent);
        el = map_find(e, c->id);
        e->tw_check |= e->tws != NULL;   /* tweens under el may be shown now */
        if (!g || !el || !slate_el_add(g, el, dirty_for(e, g), e->w, e->h)) {
            return false;
        }
        hit_touch(e, el);
        kept_take(e, el);   /* a kept root inside a group: no longer a scene left */
        if (kept_top(e, g) >= 0) {   /* caches brought into a kept scene count in its bytes */
            kept_trim(e);
        }
        return true;
    case SLATE_CMD_REMOVE:
        el = map_find(e, c->id);
        if (!el || el->parent == e->screen || el == e->scene || (e->tr.on && el == e->tr.p.from)) {
            return false;   /* a scene root leaves the screen through SHOW */
        }
        hit_touch(e, el);
        bool in_kept = el->parent && kept_top(e, el) >= 0;
        if (in_kept) {
            slate_cache_drop_tree(el);   /* out of a kept scene: its caches go with it, out of reach otherwise */
        }
        slate_el_remove(el, el->parent ? dirty_for(e, el) : NULL, e->w, e->h);
        if (in_kept) {
            kept_recount(e);
            kept_publish(e);
        }
        if (e->tws) {
            for (tw_t *en = e->tws; en && el->anim; en = en->next) {
                en->removed |= en->el == el;   /* stopped after the batches unless it was added again */
            }
            e->tw_check = true;
        }
        if (e->scs) {
            sc_t *sc = sc_of(e, el);
            if (sc) {
                sc->removed = true;   /* stopped after the batches unless it was added again */
            }
        }
        return true;
    case SLATE_CMD_FREE:
        return apply_free(e, c->id);
    case SLATE_CMD_SHOW:
        if (!apply_show(e, c)) {
            return false;
        }
        hit_clear(e);   /* empty until the first frame of what is shown now */
        return true;
    case SLATE_CMD_PIXELS:
        return apply_pixels(e, c);
    case SLATE_CMD_TWEEN:
        return apply_tween(e, c);
    case SLATE_CMD_TWEEN_STOP: {
        tw_t *en = tw_find(e, c->id);
        if (en) {
            tw_kill(e, en, c->v.type == SLATE_VAL_BOOL && c->v.v.b);
            tw_sweep(e, NULL);
            e->tw_check = true;
        }
        return true;   /* a tween that is gone: nothing to stop */
    }
    case SLATE_CMD_EASE:
        return apply_ease(e, c);
    case SLATE_CMD_SCROLL:
        return apply_scroll(e, c);
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
    e->st.tweens_active = e->tw_live;
    bm_hal_lock_exit(e->lock);
}

static bool is_move(const slate_cmd_t *a, const slate_cmd_t *b)
{
    return a->kind == SLATE_CMD_SET && b->kind == SLATE_CMD_SET && a->id == b->id && a->prop == SLATE_PROP_X &&
           b->prop == SLATE_PROP_Y;
}

static int64_t pulse_us(const slate_engine_t *e);

/* A move (x then y) of el goes SET by SET: el is shown and has ease on x or y. */
static bool eased_move(slate_engine_t *e, const slate_el_t *el)
{
    if (!el->anim) {
        return false;
    }
    ez_t *z = ez_find(e, el);
    return z && (z->mask & ((1u << SLATE_PROP_X) | (1u << SLATE_PROP_Y))) && shown(e, el);
}

/* Every delivered batch, oldest first. pulse: the wake-up's pulse (its tick comes at that pulse's time). */
static void apply_all(slate_engine_t *e, bool pulse)
{
    const slate_cmd_t *c;
    size_t n;
    uint32_t batches = 0, cmds = 0, errors = 0;
    e->apply_pulse = pulse;
    while ((n = slate_cmd_take(e->cmds, &c)) > 0) {
        for (size_t i = 0; i < n; i++) {
            slate_el_t *el;
            if (i + 1 < n && is_move(&c[i], &c[i + 1]) && !((el = map_find(e, c[i].id)) && eased_move(e, el))) {
                /* x then y of one element (set(x: .., y: ..)): one move, so the box in between is not redrawn (an
                   element with ease on x or y: one SET after the other, each may start a tween) */
                bool ok_x = false, ok_y = false;
                if (el && el->anim) {
                    tw_drop_prop(e, el, SLATE_PROP_X, NULL);   /* the write stops the tweens on x and y */
                    tw_drop_prop(e, el, SLATE_PROP_Y, NULL);
                }
                if (el) {
                    hit_touch(e, el);
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
    if (e->tw_check) {
        e->tw_check = false;
        /* before publish: who waits for the batches sees the tweens as they are. On the ticks' time base: the pulse
           that comes with these batches (its tick follows), else now; never before the last tick. */
        tw_sync(e, pulse && e->has_te ? pulse_us(e) : now_us());
    }
    if (batches && e->scs) {
        sc_sync(e, pulse && e->has_te ? pulse_us(e) : now_us());   /* shown or not, the limits; same time base */
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
    if (e->dbg_nocache == 1) {
        e->dbg_nocache = 2;
        printf("slate: a cached Scroll's content does not fit the cache: it is composed directly\n");
        any = true;
    }
    if (e->dbg_hitnomem == 1) {
        e->dbg_hitnomem = 2;
        printf("slate: no memory for the touch table: touches use the previous one\n");
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
    if (scene && e->hit) {
        e->hit_due = true;   /* the table follows the frame composed (hit_sync, after it) */
        e->hit_retry_us = 0;
    }
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
        if (st.scroll_nocache > 0 && e->dbg_nocache == 0) {
            e->dbg_nocache = 1;   /* printed by debug_print(), once */
        }
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
    bool off = failed(e) || load(&e->paused);
    bool anim = e->tw_run > 0 || e->tr.on || e->sc_run > 0;
    if ((e->dirty.n > 0 || anim) && !off) {
        until = e->has_te ? e->dirty_since_us + (int64_t)SLATE_TE_FALLBACK_MS * 1000
                          : e->last_frame_us + e->period_us;
    } else if (anim) {
        until = anim_next_end(e);   /* no frames: the tweens and the transition end on time all the same (step) */
    }
#if SLATE_DEBUG
    if (slate_debug_due(&e->dbg) < until) {
        until = slate_debug_due(&e->dbg);
    }
#endif
    if (e->scratch && e->scratch_full_us + SECOND_US < until) {
        until = e->scratch_full_us + SECOND_US;   /* scratch_trim */
    }
    if (e->sc_wake < until) {
        until = e->sc_wake;   /* a scroll's bars end their hold: their fade starts */
    }
    if (e->hit_retry_us && e->hit_due && e->hit_retry_us < until && !e->tr.on &&
        !(e->dirty.n > 0 && !failed(e) && !load(&e->paused))) {
        /* a hit table refused memory: its next try, only when hit_sync can act then (no transition, no frame to
           come first: the end of a transition and a frame give their own wake-ups and clear the wait) */
        until = e->hit_retry_us;
    }
    if (until == INT64_MAX) {
        return UINT32_MAX;
    }
    int64_t d = until - now_us();
    return d <= 0 ? 0 : (uint32_t)((d + 999) / 1000);
}

/* The time of the last TE pulse. */
static int64_t pulse_us(const slate_engine_t *e)
{
    int64_t now = now_us();
    return now - (int64_t)(uint32_t)((uint32_t)now - __atomic_load_n(&e->te.last_us, __ATOMIC_RELAXED));
}

/* One wake-up of the task, after the commands: the tweens advance and a frame goes when one is due. */
static void step(slate_engine_t *e, bool pulse)
{
    if (e->scs && e->sc_wake <= now_us()) {
        sc_count(e, now_us() < e->tw_last_us ? e->tw_last_us : now_us());   /* a hold ended: those bars fade now */
    }
    bool anim = e->tw_run > 0 || e->tr.on || e->sc_run > 0;
    if ((e->dirty.n == 0 && !anim) || failed(e) || load(&e->paused)) {
        /* nothing to draw, the panel stopped answering or sleeps: no frame (the commands are still applied) */
        store(&e->want_vsync, 0);
        swap(&e->vsync_flag, 0);   /* a pulse that came meanwhile does not start the next frame */
        if (anim && anim_next_end(e) <= now_us()) {
            anim_tick(e, now_us());   /* without frames: a tween, a transition still ends on time (its done) */
        }
        return;
    }
    if (e->has_te) {
        if (!load(&e->want_vsync)) {
            store(&e->want_vsync, 1);   /* from now on the pulses wake the task */
            e->dirty_since_us = now_us();
        }
        bool late = now_us() - e->dirty_since_us >= (int64_t)SLATE_TE_FALLBACK_MS * 1000;
        if (pulse || late) {
            if (anim) {
                anim_tick(e, pulse ? pulse_us(e) : now_us());   /* the time of the pulse that starts the frame */
            }
            if (e->dirty.n > 0) {
                if (!pulse) {
                    /* no pulse for a long while (the TE pin taken by another object, a panel asleep): frame anyway */
                    bm_hal_lock_enter(e->lock);
                    e->st.te_fallbacks++;
                    bm_hal_lock_exit(e->lock);
                }
                frame(e);
            }
            e->dirty_since_us = now_us();   /* still dirty (retry, abort) or animated: wait for the next pulse again */
        }
    } else if (now_us() - e->last_frame_us >= e->period_us) {
        int64_t t = now_us();
        if (anim) {
            anim_tick(e, t);   /* the time the frame starts */
        }
        if (e->dirty.n > 0) {
            frame(e);
        } else {
            e->last_frame_us = t;   /* a tween waiting (delay): the next tick a period later */
        }
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
        keep_sync(e);   /* lowered limits free at once, even without a batch */
        apply_all(e, pulse);
        sync_images(e);
        if (swap(&e->redraw_flag, 0) && e->drawn) {
            dirty_all(e);   /* the panel lost its picture: the whole screen again */
        }
#if SLATE_DEBUG
        debug_tick(e);
#endif
        step(e, pulse);
        hit_sync(e);
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
    for (tw_t *en = e->tws; en;) {
        tw_t *next = en->next;
        slate_tween_free(en->t);
        ext_free(en);
        en = next;
    }
    e->tws = e->tws_tail = NULL;
    while (e->scs) {
        sc_drop(e, e->scs);
    }
    ext_free(e->kept);   /* the roots are freed with the map, their caches with them */
    e->kept = NULL;
    e->kept_n = e->kept_cap = 0;
    for (ez_t *z = e->ezs; z;) {
        ez_t *next = z->next;
        ext_free(z);
        z = next;
    }
    e->ezs = NULL;
    ext_free(e->fin);
    e->fin = NULL;
    slate_hit_pub_put(e->hit);   /* and the block given and not taken yet */
    e->hit = NULL;
    bm_hal_lock_enter(e->lock);
    slate_hit_pub_t *hn = e->hit_new;
    e->hit_new = NULL;
    bm_hal_lock_exit(e->lock);
    slate_hit_pub_put(hn);
    slate_hit_ids_free(&e->hit_ids);
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

/* ---- the image loader's reclaim (images.h, Memory) ---- */

static slate_engine_t *s_reclaim_e;   /* the running engine for reclaim() (NULL from the start of its stop) */
static int s_reclaim_users;           /* reclaim() calls under way */

/* From the image loader's task, out of memory: asks the engine to drop the caches of the scenes left (on its own
   task) and waits for its answer, at most SLATE_RECLAIM_WAIT_MS (by the clock, and by turns of at least SLATE_NAP_MS:
   a frozen test clock or a wait rounded to 0 ticks cannot make it longer). true: the engine freed something, or did
   not answer in time (the loader tries once more); false: no engine runs, or nothing was kept. e is read only
   before the wait: from then on only the queue (alive until s_reclaim_users is back to 0, slate_engine_stop). */
static bool reclaim(void)
{
    __atomic_add_fetch(&s_reclaim_users, 1, __ATOMIC_SEQ_CST);
    slate_engine_t *e = __atomic_load_n(&s_reclaim_e, __ATOMIC_SEQ_CST);
    bool retry = false;
    if (e && !load(&e->stop_flag)) {
        bm_hal_queue_t *ack = e->ack;
        uint32_t req = __atomic_add_fetch(&e->reclaim_req, 2, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&e->keep_flag, KEEP_RECLAIM, __ATOMIC_SEQ_CST);
        slate_engine_kick(e);
        e = NULL;
        retry = true;   /* no answer in time: try anyway */
        int64_t until = now_us() + (int64_t)SLATE_RECLAIM_WAIT_MS * 1000;
        for (int turns = SLATE_RECLAIM_WAIT_MS / SLATE_NAP_MS + 1; turns > 0; turns--) {
            int64_t left = until - now_us();
            if (left <= 0) {
                break;
            }
            uint32_t got, ms = (uint32_t)((left + 999) / 1000);
            if (!bm_hal_queue_receive(ack, &got, ms < SLATE_NAP_MS ? SLATE_NAP_MS : ms)) {
                continue;
            }
            if ((int32_t)((got & ~1u) - req) >= 0) {   /* answers to older requests (timed out) are skipped */
                retry = (got & 1u) != 0;
                break;
            }
        }
    }
    __atomic_sub_fetch(&s_reclaim_users, 1, __ATOMIC_SEQ_CST);
    return retry;
}

/* ---- PHP task ---- */

static void release(slate_engine_t *e)
{
    bm_hal_free(e->dq);
    bm_hal_free(e->scq);
    slate_cmdbuf_free(e->cmds);
    slate_queue_put(e->wake);
    slate_queue_put(e->freed);
    slate_queue_put(e->ctl);
    slate_queue_put(e->ack);
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
    e->tw_cb = s_tw_cb;
    e->tw_arg = s_tw_arg;
    e->slot[0] = (band_slot_t){ .e = e, .i = 0 };
    e->slot[1] = (band_slot_t){ .e = e, .i = 1 };
    e->wake = slate_queue_get();
    e->freed = slate_queue_get();
    e->ctl = slate_queue_get();
    e->ack = slate_queue_get();
    e->lock = slate_lock_get();
    e->cmds = slate_cmdbuf_new();
    if (!e->wake || !e->freed || !e->ctl || !e->ack || !e->lock || !e->cmds) {
        release(e);
        return NULL;
    }
    e->cache_budget = probe_budget();
    e->st.cache_budget = e->cache_budget;
    e->last_frame_us = now_us() - e->period_us;
    e->prev_scene_us = INT64_MIN / 2;
    e->last_scene_us = INT64_MIN / 2;
    e->sc_wake = INT64_MAX;
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
    slate_images_set_reclaim(reclaim);   /* code, set once and for all: it finds the engine in s_reclaim_e */
    __atomic_store_n(&s_reclaim_e, e, __ATOMIC_SEQ_CST);
    return e;
}

bool slate_engine_stop(slate_engine_t *e)
{
    if (!e) {
        return true;
    }
    /* No reclaim reaches e from now on; one under way ends within SLATE_RECLAIM_WAIT_MS (the task, still running,
       answers it, or it times out), and e stays until then: waited for by the clock and by turns of SLATE_NAP_MS
       (each a real block: on e->ctl, where nothing comes before the stop), so the loader gets the CPU. One that did
       not end after a second more (a starved loader) leaves e and its queues allocated, as a stuck task does. */
    __atomic_store_n(&s_reclaim_e, NULL, __ATOMIC_SEQ_CST);
    int64_t rdl = now_us() + (int64_t)SLATE_RECLAIM_STOP_MS * 1000;
    for (int turns = SLATE_RECLAIM_STOP_MS / SLATE_NAP_MS + 1;
         __atomic_load_n(&s_reclaim_users, __ATOMIC_SEQ_CST) > 0 && turns > 0 && now_us() < rdl; turns--) {
        uint32_t m;
        bm_hal_queue_receive(e->ctl, &m, SLATE_NAP_MS);
    }
    e->reclaim_stuck = __atomic_load_n(&s_reclaim_users, __ATOMIC_SEQ_CST) > 0;
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
    if (e->lost[0] || e->lost[1] || e->reclaim_stuck) {
        /* A late done may still write e->busy and send to e->freed, a late reclaim read e or e->ack: e and its
           queues stay allocated (leaked). The reason stays readable (slate_engine_error): a lost band failed with
           ERR_TIMEOUT already; a stuck reclaim alone says so. */
        if (e->reclaim_stuck) {
            fail(e, ERR_RECLAIM);
        }
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
    out->anim_ticks = __atomic_load_n(&e->st.anim_ticks, __ATOMIC_RELAXED);   /* counted without the lock */
}

void slate_engine_set_cache_budget(slate_engine_t *e, size_t bytes)
{
    __atomic_store_n(&e->cache_budget, bytes, __ATOMIC_RELAXED);
}

void slate_engine_set_keep(slate_engine_t *e, int scenes, size_t budget_bytes)
{
    __atomic_store_n(&e->keep_scenes_req, scenes < 0 ? 0 : scenes, __ATOMIC_SEQ_CST);
    __atomic_store_n(&e->keep_budget_req, budget_bytes, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&e->keep_flag, KEEP_LIMITS, __ATOMIC_SEQ_CST);
    slate_engine_kick(e);
}

bool slate_engine_tween_done(slate_engine_t *e, uint32_t *tween_id)
{
    bool got = false;
    bm_hal_lock_enter(e->lock);
    if (e->dq_n > 0) {
        *tween_id = e->dq[e->dq_head];
        e->dq_head = (e->dq_head + 1) % e->dq_cap;
        e->dq_n--;
        got = true;
    }
    bm_hal_lock_exit(e->lock);
    return got;
}

bool slate_engine_transition_done(slate_engine_t *e, uint32_t *from_root, uint32_t *to_root)
{
    bool got = false;
    bm_hal_lock_enter(e->lock);
    if (e->trq_n > 0) {
        *from_root = e->trq[e->trq_head].from;
        *to_root = e->trq[e->trq_head].to;
        e->trq_head = (e->trq_head + 1) % TR_DONE_MAX;
        e->trq_n--;
        got = true;
    }
    bm_hal_lock_exit(e->lock);
    return got;
}

bool slate_engine_in_transition(slate_engine_t *e)
{
    return load(&e->tr_flag) != 0;
}

bool slate_engine_scroll_done(slate_engine_t *e, uint32_t *el_id, int *x, int *y)
{
    bool got = false;
    bm_hal_lock_enter(e->lock);
    if (e->scq_n > 0) {
        sce_t en = e->scq[e->scq_head];
        *el_id = en.id;
        *x = en.x;
        *y = en.y;
        e->scq_head = (e->scq_head + 1) % e->scq_cap;
        e->scq_n--;
        got = true;
    }
    bm_hal_lock_exit(e->lock);
    return got;
}

void slate_engine_set_tween_listener(void (*cb)(void *arg), void *arg)
{
    s_tw_cb = cb;
    s_tw_arg = arg;
}

void slate_engine_set_hit_pub(slate_engine_t *e, slate_hit_pub_t *p)
{
    slate_hit_pub_ref(p);
    bm_hal_lock_enter(e->lock);
    slate_hit_pub_t *old = e->hit_new;   /* given before and not taken yet */
    e->hit_new = p;
    store(&e->hit_flag, 1);
    bm_hal_lock_exit(e->lock);
    slate_hit_pub_put(old);
    slate_engine_kick(e);
}
