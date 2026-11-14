/*
   +----------------------------------------------------------------------+
   | php-baremetal: ext-slate                                             |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/*
 * Scroll in PHP: the PHP side of a scroll container (touch(), scrollTo(), its position), the enum ScrollAxis and the
 * event ScrollEnded. Its properties (and their checks) are php_slate_elements.c's; the physics, the frames and the
 * ends are the engine's (engine.h, Scrolls).
 *
 * Commands. A Scroll is a GROUP made a scroll by a SCROLL CONFIG, sent right after its NEW and again (with every
 * value) when $axis, $bounce, $snapX, $snapY, $snapMax or $indicators change. A write of $scrollX/$scrollY is a JUMP
 * (both axes: the other one where it is); touch() sends PRESS, DRAG and RELEASE with the time it runs (bm_hal_now_us(), the
 * clock of the engine), the touch routing (php_slate_touch.c) the same with the time of each touch event, and CANCEL
 * for a touch it ends without its Released; scrollTo() is a TWEEN of SCROLL_X and SCROLL_Y through the Tween of php_slate_anim.c.
 *
 * Position. The engine publishes the position it draws into a small block shared with this object
 * (slate_scroll_pub_t, cmd.h, made at the first realization): reading $scrollX/$scrollY gives it, without a lock.
 * After a write (or before the first frame) the value written is read until the engine has applied that JUMP (the
 * block's seq reaches the JUMP's): the slots hold the value read, refreshed at each read.
 *
 * Touch. The view box on the screen comes from the PHP properties: the x, y of the scroll and of its ancestors, minus
 * the position of the ancestors that are scrolls. A Pressed inside starts a touch; the move past 8 px (on the axis
 * allowed; with Both, the distance) starts the drag, and that point is the first DRAG (the engine anchors there: no
 * jump). A Pressed that stops a motion makes the whole touch a catch: its Moved and Released are the scroll's too
 * (true; a drag may still start from it). The same state machine serves the touch routing (sl_scroll_take,
 * sl_scroll_take_drag, sl_scroll_event, sl_scroll_drop: a Scroll is touchable by default); while it drives a touch
 * (autod), touch() does nothing and returns false, so no touch is fed twice. The engine stops a running scrollTo() on a PRESS, a JUMP and a new set-up without a done: its Tween is
 * marked stopped here (sl_tween_silenced).
 *
 * Ends. The engine queues an end (element id, position) when a motion ends by itself and calls the tween listener
 * (php_slate_anim.c), which drains them here on the engine's task: one ScrollEnded event each (12 bytes: the id and
 * the position) for a Scroll still alive (a table of the alive ids, under a lock); the filler finds the object. An end
 * of a Scroll freed between its event and the delivery carries a stand-in (an empty Scroll, never drawn): the event
 * cannot be taken back from the queue. An end lost (a full event pool) changes nothing in PHP.
 */
#include <math.h>
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#include "bm_events_hal.h"
#include "event_bus_php.h"
#include "php_slate.h"

zend_class_entry *sl_scroll_ce, *sl_scroll_axis_ce, *sl_scroll_ended_ce;

#define DRAG_THRESHOLD 8.0f   /* px before a touch is a drag */
#define POS_LIMIT 32767.0     /* positions sent are cut here (the engine's limits are within): a finite double past the
                                 float range would become inf, which the engine drops */

static float pos_f(double v)
{
    return (float)(v < -POS_LIMIT ? -POS_LIMIT : v > POS_LIMIT ? POS_LIMIT : v);
}

/* The PHP side of a Scroll (sl_obj_t.sc). */
typedef struct sl_scroll {
    slate_scroll_pub_t *pub;   /* where the engine publishes (a reference); NULL: never realized */
    uint32_t seq;              /* the last seq sent (JUMP, CONFIG) */
    uint32_t wseq;             /* the seq of the JUMP (or the first CONFIG) the slots wait for; 0: not sent yet */
    bool local;                /* the slots hold what was written, until the engine applies wseq */
    uint32_t to_tween;         /* the id of its last scrollTo() (0: none) */
    bool down;                 /* a Pressed inside, not released */
    bool dragging;             /* past the threshold, DRAGs sent */
    bool owned;                /* the touch is the scroll's (a drag started, or its Pressed caught a motion): its Moved
                                  and Released return true, never a tap */
    bool cut;                  /* the engine ended this touch (a jump, a scrollTo, a new set-up, the scroll off the
                                  screen): nothing more is sent until the next Pressed */
    float px, py;              /* the Pressed point */
    bool autod;                /* the touch routing (php_slate_touch.c) drives this touch: touch() ignores it */
    bool hid;                  /* seen off the screen during the touch (removed, hidden, its page left): the engine
                                  does not end a drag for a hidden scroll, so its end is a CANCEL */
} sl_scroll_t;

static const char *const axis_names[] = { "Both", "Vertical", "Horizontal" };   /* slate_scroll_axis_t order */

/* ---- the alive table and the event (shared with the engine's task) ---- */

typedef struct {
    uint32_t id;
    sl_obj_t *o;               /* read on the PHP task only */
} sl_alive_t;

static bm_hal_lock_t *s_lock;  /* the table, s_accepting, s_tag; created once, never freed */
static sl_alive_t *s_alive;    /* bm_hal_alloc'd (it outlives requests: the Scroll objects of a request may be freed
                                  after its RSHUTDOWN) */
static uint32_t s_alive_n, s_alive_cap;
static bool s_accepting;       /* a request runs with the event set up */
static uint16_t s_tag;
static bool s_ready;           /* PHP task: the event is set up in this request */

static bm_hal_lock_t *lock(void)
{
    bm_hal_lock_t *l = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
    bm_hal_lock_enter(l);
    return l;
}

/* Index of id in the table, or -1. Under the lock (or on the PHP task, the only writer). */
static int alive_find(uint32_t id)
{
    for (uint32_t i = 0; i < s_alive_n; i++) {
        if (s_alive[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* o into the table (once). false with an exception. PHP task. */
static bool alive_add(sl_obj_t *o)
{
    if (alive_find(o->id) >= 0) {
        return true;
    }
    if (s_alive_n == s_alive_cap) {
        uint32_t cap = s_alive_cap ? s_alive_cap * 2 : 16;
        sl_alive_t *n = bm_hal_alloc(cap * sizeof *n, BM_HAL_MEM_EXTERNAL);
        if (!n) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: out of memory for the scroll", 0);
            return false;
        }
        bm_hal_lock_t *l = lock();
        if (s_alive_n) {
            memcpy(n, s_alive, s_alive_n * sizeof *n);
        }
        sl_alive_t *old = s_alive;
        s_alive = n;
        s_alive_cap = cap;
        bm_hal_lock_exit(l);
        if (old) {
            bm_hal_free(old);
        }
    }
    bm_hal_lock_t *l = lock();
    s_alive[s_alive_n++] = (sl_alive_t){ o->id, o };
    bm_hal_lock_exit(l);
    return true;
}

static void alive_del(uint32_t id)
{
    if (!__atomic_load_n(&s_lock, __ATOMIC_ACQUIRE)) {
        return;
    }
    bm_hal_lock_t *l = lock();
    int i = alive_find(id);
    if (i >= 0) {
        s_alive[i] = s_alive[--s_alive_n];
    }
    bm_hal_lock_exit(l);
}

/* On the engine task only (its tween listener). No in-flight guard like the tweens' s_emitting: RSHUTDOWN stops and
   joins the engine (sl_stop) before sl_scroll_rshutdown, so no drain can run while the state below is torn down;
   that order is required. */
void sl_scroll_pull(slate_engine_t *e)
{
    if (!e || !__atomic_load_n(&s_lock, __ATOMIC_ACQUIRE)) {
        return;
    }
    uint32_t id;
    int x, y;
    while (slate_engine_scroll_done(e, &id, &x, &y)) {
        bm_hal_lock_t *l = lock();
        bool emit = s_accepting && alive_find(id) >= 0;   /* a Scroll freed meanwhile: its end goes */
        uint16_t tag = s_tag;
        bm_hal_lock_exit(l);
        if (emit) {
            int32_t d[3] = { (int32_t)id, x, y };
            bm_events_emit_data(tag, NULL, d, sizeof d);   /* dropped (a full pool): lost, nothing to catch up */
        }
    }
}

/* The filler of ScrollEnded (PHP task, before the delivery). */
static void fill_end(zend_object *ev, zend_object *dev, const evt_t *slot)
{
    (void)dev;
    size_t len;
    const void *d = bm_events_slot_data(slot, &len);
    int32_t v[3] = { 0, 0, 0 };
    if (d && len == sizeof v) {
        memcpy(v, d, sizeof v);
    }
    int i = alive_find((uint32_t)v[0]);
    zval zs;
    if (i >= 0) {
        ZVAL_OBJ(&zs, &s_alive[i].o->std);
        bm_events_set_zval(ev, sl_scroll_ended_ce, "scroll", &zs);   /* the event's own reference */
    } else {
        object_init_ex(&zs, sl_scroll_ce);   /* freed after its end was queued: a stand-in, so the event is whole */
        bm_events_set_zval(ev, sl_scroll_ended_ce, "scroll", &zs);
        zval_ptr_dtor(&zs);
    }
    bm_events_set_long(ev, sl_scroll_ended_ce, "x", v[1]);
    bm_events_set_long(ev, sl_scroll_ended_ce, "y", v[2]);
}

/* The first realized Scroll of the request: the lock (once), the event class as a source of ext-events. false with
   an exception. */
static bool ready(void)
{
    if (s_ready) {
        return true;
    }
    if (!s_lock) {
        bm_hal_lock_t *l = bm_hal_lock_create();
        if (!l) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: no memory for the scroll", 0);
            return false;
        }
        __atomic_store_n(&s_lock, l, __ATOMIC_RELEASE);
    }
    uint16_t tag;
    if (!bm_events_source_tag(sl_scroll_ended_ce->name, &tag)) {
        if (!EG(exception)) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Slate: cannot raise %s: too many event sources",
                                    ZSTR_VAL(sl_scroll_ended_ce->name));
        }
        return false;
    }
    bm_events_set_filler(tag, fill_end);
    bm_hal_lock_t *l = lock();
    s_tag = tag;
    s_accepting = true;
    bm_hal_lock_exit(l);
    s_ready = true;
    return true;
}

void sl_scroll_rshutdown(void)
{
    if (!s_ready) {
        return;
    }
    bm_hal_lock_t *l = lock();
    s_accepting = false;
    bm_hal_lock_exit(l);
    bm_events_set_filler(s_tag, NULL);
    s_ready = false;
}

/* ---- the object ---- */

uint8_t sl_scroll_axis_of(const zval *v)
{
    if (!v) {
        return SLATE_SCROLL_BOTH;
    }
    ZVAL_DEREF(v);
    if (Z_TYPE_P(v) == IS_OBJECT && Z_OBJCE_P(v) == sl_scroll_axis_ce) {
        zval *name = zend_enum_fetch_case_name(Z_OBJ_P(v));
        for (int i = 0; i < 3; i++) {
            if (Z_TYPE_P(name) == IS_STRING && strcmp(Z_STRVAL_P(name), axis_names[i]) == 0) {
                return (uint8_t)i;
            }
        }
    }
    return SLATE_SCROLL_BOTH;
}

void sl_scroll_init(sl_obj_t *o)
{
    o->sc = ecalloc(1, sizeof *o->sc);
}

void sl_scroll_free(sl_obj_t *o)
{
    alive_del(o->id);
    slate_scroll_pub_put(o->sc->pub);   /* the engine and the commands in flight hold their own */
    efree(o->sc);
    o->sc = NULL;
}

static void set_slot(sl_obj_t *o, const char *name, zend_long v)
{
    zval *slot = sl_slot(o, name);
    if (slot) {
        zval_ptr_dtor(slot);
        ZVAL_LONG(slot, v);
    }
}

void sl_scroll_sync(sl_obj_t *o)
{
    sl_scroll_t *sc = o->sc;
    if (!sc || !sc->pub) {
        return;   /* never drawn: the slots are the position */
    }
    uint32_t seq = slate_scroll_pub_seq(sc->pub);   /* before the position (cmd.h) */
    if (sc->local && sc->wseq != 0 && (int32_t)(seq - sc->wseq) >= 0) {
        sc->local = false;   /* the engine has applied the write: from now on its position */
    }
    if (sc->local) {
        return;
    }
    int x, y;
    slate_scroll_pub_pos(sc->pub, &x, &y);
    set_slot(o, "scrollX", x);
    set_slot(o, "scrollY", y);
}

/* Pushes a SCROLL command of o. */
static bool push_scroll(sl_obj_t *o, slate_scroll_cmd_t *d)
{
    slate_cmd_t c = { .kind = SLATE_CMD_SCROLL, .id = o->id, .data = d };
    return sl_push(&c);
}

/* A JUMP, a scrollTo or a new set-up disarms the engine's touch (scroll.c stop_all): the PHP touch follows. */
static void cut_touch(sl_obj_t *o)
{
    if (o->sc->down) {
        o->sc->dragging = false;
        o->sc->cut = true;
    }
}

/* The engine stops the running scrollTo() of o without a done: its Tween is marked stopped. */
static void silence(sl_obj_t *o)
{
    if (o->sc->to_tween) {
        sl_tween_silenced(o->sc->to_tween);
        o->sc->to_tween = 0;
    }
}

static bool push_config(sl_obj_t *o)
{
    sl_scroll_t *sc = o->sc;
    slate_scroll_cmd_t d = {
        .op = SLATE_SCROLL_OP_CONFIG,
        .axis = sl_scroll_axis_of(sl_slot(o, "axis")),
        .bounce = sl_slot_true(o, "bounce"),
        .indicators = sl_slot_true(o, "indicators"),
        .snap_x = (int32_t)sl_slot_num(o, "snapX"),
        .snap_y = (int32_t)sl_slot_num(o, "snapY"),
        .snap_max = (int32_t)sl_slot_num(o, "snapMax"),
        .pub = sc->pub,
        .seq = ++sc->seq,
    };
    return push_scroll(o, &d);
}

static bool push_jump(sl_obj_t *o)
{
    sl_scroll_t *sc = o->sc;
    slate_scroll_cmd_t d = {
        .op = SLATE_SCROLL_OP_JUMP,
        .x = pos_f(sl_slot_num(o, "scrollX")),
        .y = pos_f(sl_slot_num(o, "scrollY")),
        .seq = ++sc->seq,
    };
    if (!push_scroll(o, &d)) {
        return false;
    }
    sc->wseq = sc->seq;
    sc->local = true;
    return true;
}

bool sl_scroll_realize_config(sl_obj_t *o)
{
    sl_scroll_t *sc = o->sc;
    if (!ready() || !alive_add(o)) {
        return false;
    }
    if (!sc->pub) {
        sc->pub = slate_scroll_pub_new();
        if (!sc->pub) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: out of memory for the scroll", 0);
            return false;
        }
    }
    sl_scroll_sync(o);     /* the position of an earlier Slate (or the one written): the JUMP after the children */
    sc->local = true;
    sc->wseq = 0;
    sc->down = sc->dragging = sc->owned = sc->cut = sc->autod = sc->hid = false;
    return push_config(o);
}

bool sl_scroll_realize_position(sl_obj_t *o)
{
    sl_scroll_t *sc = o->sc;
    if (sl_slot_num(o, "scrollX") != 0 || sl_slot_num(o, "scrollY") != 0) {
        return push_jump(o);
    }
    sc->wseq = sc->seq;    /* at 0: the CONFIG of a new scroll publishes 0, 0 */
    return true;
}

bool sl_scroll_jumped(sl_obj_t *o)
{
    sl_scroll_t *sc = o->sc;
    if (!sl_realized(o)) {
        sc->local = true;  /* sent when it is realized */
        sc->wseq = 0;
        return true;
    }
    silence(o);
    cut_touch(o);
    return push_jump(o);
}

bool sl_scroll_configured(sl_obj_t *o, bool setup)
{
    if (!sl_realized(o)) {
        return true;       /* sent when it is realized */
    }
    if (setup) {
        silence(o);
        cut_touch(o);
    }
    return push_config(o);
}

/* ---- touch() ---- */

static zend_class_entry *s_touch_ce[4];   /* Pressed, Moved, Released, Gesture of ext-input */
static zend_class_entry *s_touch_event_ce; /* their base, TouchEvent */

static zend_class_entry *touch_class(int i)
{
    static const char *const names[4] = {
        "baremetal\\input\\pressed", "baremetal\\input\\moved", "baremetal\\input\\released",
        "baremetal\\input\\gesture",
    };
    if (!s_touch_ce[i]) {
        s_touch_ce[i] = zend_hash_str_find_ptr(CG(class_table), names[i], strlen(names[i]));
    }
    return s_touch_ce[i];
}

/* Whether o is drawn in the scene the Slate shows (not during a transition: the touch goes to no scene then), visible
   with its ancestors; *box: the part of its view on the screen, cut by every clipping ancestor (the views of the
   scrolls it is in, the boxes of groups with $clip); empty when none of it shows. The PHP values are used: an ancestor
   that animates is taken at its final x, y. An element more than DEPTH_MAX levels below the scene (a child of the
   scene is at level 1) is taken as not on screen: false, so touch() ignores the touch (docs/usage/scroll.md). */
#define DEPTH_MAX 64
typedef struct { double x0, y0, x1, y1; } box_t;

static bool on_screen(sl_obj_t *o, box_t *box)
{
    zend_object *shown = sl_shown_scene();
    if (!shown || !sl_realized(o) || sl_trans_running()) {
        return false;
    }
    sl_obj_t *chain[DEPTH_MAX];
    int n = 0;
    sl_obj_t *a = o;
    for (; a && a->base != SL_SCENE; a = a->parent) {
        if (!sl_slot_true(a, "visible") || n == DEPTH_MAX) {
            return false;
        }
        chain[n++] = a;
    }
    if (!a || &a->std != shown) {
        return false;
    }
    /* from the scene down: each element's origin, and the clip of the clipping ones */
    box_t clip = { -1e9, -1e9, 1e9, 1e9 };
    double ox = 0, oy = 0;
    for (int i = n - 1; i >= 0; i--) {
        sl_obj_t *e = chain[i];
        ox += sl_slot_num(e, "x");
        oy += sl_slot_num(e, "y");
        double w = sl_slot_num(e, "w"), h = sl_slot_num(e, "h");
        /* as the engine clips (slate_el_clips): a scroll always, a group with $clip only with a box (w and h > 0) */
        if (e->base == SL_SCROLL || (sl_slot_true(e, "clip") && w > 0 && h > 0)) {
            double x1 = ox + w, y1 = oy + h;
            clip.x0 = ox > clip.x0 ? ox : clip.x0;
            clip.y0 = oy > clip.y0 ? oy : clip.y0;
            clip.x1 = x1 < clip.x1 ? x1 : clip.x1;
            clip.y1 = y1 < clip.y1 ? y1 : clip.y1;
        }
        if (e != o && e->base == SL_SCROLL) {
            sl_scroll_sync(e);   /* its children are drawn shifted by its position */
            ox -= sl_slot_num(e, "scrollX");
            oy -= sl_slot_num(e, "scrollY");
        }
    }
    *box = clip;   /* o is a scroll: its own view is the last clip */
    return true;
}

static float event_coord(zend_object *ev, const char *name)
{
    zval rv;
    zval *v = zend_read_property(ev->ce, ev, name, strlen(name), 1, &rv);
    return v ? (float)zval_get_double(v) : 0.0f;
}

static bool push_touch(sl_obj_t *o, uint8_t op, float x, float y, int64_t t_us)
{
    slate_scroll_cmd_t d = { .op = op, .x = x, .y = y, .t_us = t_us };
    return push_scroll(o, &d);
}

/* Moving now: a drag, an inertia, a snap, a spring or a scrollTo (also one asked and not delivered yet). A scrollTo
   that ended in the engine whose done is not pulled yet still counts (rare): a press then is a catch and its Tween is
   marked stopped, while its TweenDone (a stand-in) and ScrollEnded are already queued. */
bool sl_scroll_moving(sl_obj_t *o)
{
    sl_scroll_t *sc = o->sc;
    return sc && ((sc->pub && slate_scroll_pub_moving(sc->pub)) || (sc->to_tween && sl_tween_running(sc->to_tween)));
}

/* ---- the touch state machine (touch() and the routing of php_slate_touch.c) ---- */

/* A new touch pressed at (x, y) at t_us: PRESS, which stops a motion (the whole touch is then a catch). false with
   an exception. */
static bool tm_press(sl_obj_t *o, float x, float y, int64_t t_us)
{
    sl_scroll_t *sc = o->sc;
    sc->down = sc->dragging = sc->owned = sc->cut = sc->hid = false;
    bool moving = sl_scroll_moving(o);
    if (!push_touch(o, SLATE_SCROLL_OP_PRESS, x, y, t_us)) {
        return false;
    }
    silence(o);   /* the PRESS stops a scrollTo without a done */
    sc->down = true;
    sc->owned = moving;   /* a catch: the whole touch is the scroll's, never a tap */
    sc->px = x;
    sc->py = y;
    return true;
}

/* A Moved of the touch: 1 when it is the scroll's (a drag, or a catch), 0 when not (yet), -1 with an exception.
   shown: the scroll is on the screen now (else the engine ended the drag: nothing more is sent). */
static int tm_move(sl_obj_t *o, float x, float y, int64_t t_us, bool shown)
{
    sl_scroll_t *sc = o->sc;
    if (!shown) {
        sc->dragging = false;
        sc->cut = sc->hid = true;
    }
    if (!sc->dragging) {
        if (sc->cut) {
            return sc->owned;   /* the engine ended this touch: no new drag until a Pressed */
        }
        float dx = x - sc->px, dy = y - sc->py, dist;
        switch (sl_scroll_axis_of(sl_slot(o, "axis"))) {
        case SLATE_SCROLL_V:
            dist = fabsf(dy);
            break;
        case SLATE_SCROLL_H:
            dist = fabsf(dx);
            break;
        default:
            dist = sqrtf(dx * dx + dy * dy);
            break;
        }
        if (!(dist > DRAG_THRESHOLD)) {
            return sc->owned;   /* not a drag yet: a tap, unless the press caught a motion */
        }
        sc->dragging = sc->owned = true;   /* this point is the first DRAG: the engine anchors there */
    }
    return push_touch(o, SLATE_SCROLL_OP_DRAG, x, y, t_us) ? 1 : -1;
}

/* The Released of the touch (op RELEASE), or its loss (op CANCEL: a release without velocity): 1 when the touch
   was the scroll's, 0 when not, -1 with an exception. A scroll seen off the screen during the touch (or now) gets a
   CANCEL: the engine ends a drag by itself only for a scroll removed or whose page is left, not for a hidden one
   (a CANCEL after its own end does nothing). Nothing after the engine ended the touch (a jump, a scrollTo, a new
   set-up) on the screen. */
static int tm_release(sl_obj_t *o, uint8_t op, int64_t t_us, bool shown)
{
    sl_scroll_t *sc = o->sc;
    bool off = sc->hid || !shown;
    bool owned = sc->owned, send = !sc->cut || off;
    if (off) {
        op = SLATE_SCROLL_OP_CANCEL;
    }
    sc->down = sc->dragging = sc->owned = sc->cut = sc->hid = false;
    /* also after a tap: the engine resumes a snap the press stopped, or springs back from past an edge */
    if (send && !push_touch(o, op, 0, 0, t_us)) {
        return -1;
    }
    return owned;
}

static bool shown_now(sl_obj_t *o)
{
    box_t box;
    return on_screen(o, &box);
}

bool sl_scroll_take(sl_obj_t *o, const sl_touch_t *t)
{
    if (!tm_press(o, (float)t->x, (float)t->y, t->t_us)) {
        return false;
    }
    o->sc->autod = true;   /* touch() leaves this touch alone */
    return true;
}

bool sl_scroll_take_drag(sl_obj_t *o, const sl_touch_t *press, const sl_touch_t *t)
{
    sl_scroll_t *sc = o->sc;
    if (!tm_press(o, (float)press->x, (float)press->y, press->t_us)) {
        return false;
    }
    sc->autod = true;
    sc->dragging = sc->owned = true;   /* the hand-off point is the first DRAG: the engine anchors there, no jump */
    return push_touch(o, SLATE_SCROLL_OP_DRAG, (float)t->x, (float)t->y, t->t_us);
}

bool sl_scroll_owns(const sl_obj_t *o)
{
    return o->sc && o->sc->owned;
}

bool sl_scroll_event(sl_obj_t *o, const sl_touch_t *t, bool released)
{
    sl_scroll_t *sc = o->sc;
    if (!sc->autod || !sc->down) {
        return true;
    }
    bool shown = shown_now(o);
    if (released) {
        sc->autod = false;
        return tm_release(o, SLATE_SCROLL_OP_RELEASE, t->t_us, shown) >= 0;
    }
    return tm_move(o, (float)t->x, (float)t->y, t->t_us, shown) >= 0;
}

bool sl_scroll_drop(sl_obj_t *o, bool send)
{
    sl_scroll_t *sc = o->sc;
    if (!sc || !sc->autod) {
        return true;
    }
    sc->autod = false;
    if (!sc->down) {
        return true;
    }
    if (!send) {
        sc->down = sc->dragging = sc->owned = sc->cut = sc->hid = false;
        return true;
    }
    return tm_release(o, SLATE_SCROLL_OP_CANCEL, bm_hal_now_us(), shown_now(o)) >= 0;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scroll_touch, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_OBJ_INFO(0, e, Baremetal\\Input\\TouchEvent, 0)
ZEND_END_ARG_INFO()

/* touch(): true when the touch belongs to the scroll: a drag, or a catch (a press that stopped its motion: all its
   events, Released included); see docs/usage/scroll.md. While the routing drives the scroll (a touch that hit it, or
   one it took over from an element), touch() does nothing and returns false: no touch is fed twice. */
PHP_METHOD(SlateScroll, touch)
{
    zend_object *ev;
    if (!s_touch_event_ce) {
        s_touch_event_ce = zend_hash_str_find_ptr(CG(class_table), "baremetal\\input\\touchevent",
                                                  sizeof("baremetal\\input\\touchevent") - 1);
    }
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJ_OF_CLASS(ev, s_touch_event_ce)   /* checked in every build (arginfo only in debug ones) */
    ZEND_PARSE_PARAMETERS_END();
    sl_anim_flush();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    sl_scroll_t *sc = o->sc;
    if (!sc || sc->autod) {
        RETURN_FALSE;   /* never without a state; the routing's touch */
    }
    box_t box;
    bool shown = on_screen(o, &box);
    zend_class_entry *ce = ev->ce;
    int r;
    if (ce == touch_class(0)) {   /* Pressed: a new touch */
        sc->down = sc->dragging = sc->owned = sc->cut = false;
        float x = event_coord(ev, "x"), y = event_coord(ev, "y");
        if (!shown || x < box.x0 || y < box.y0 || x >= box.x1 || y >= box.y1) {
            RETURN_FALSE;   /* not shown, or outside the part of the view on the screen: not its touch */
        }
        if (!tm_press(o, x, y, bm_hal_now_us())) {
            RETURN_THROWS();
        }
        RETURN_BOOL(sc->owned);
    }
    if (!sc->down || (ce != touch_class(1) && ce != touch_class(2))) {
        RETURN_FALSE;   /* no touch of its own, or a Gesture */
    }
    if (ce == touch_class(1)) {   /* Moved */
        r = tm_move(o, event_coord(ev, "x"), event_coord(ev, "y"), bm_hal_now_us(), shown);
    } else {                      /* Released: off the screen, a CANCEL (tm_release) */
        r = tm_release(o, SLATE_SCROLL_OP_RELEASE, bm_hal_now_us(), shown);
    }
    if (r < 0) {
        RETURN_THROWS();
    }
    RETURN_BOOL(r);
}

/* ---- scrollTo() ---- */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_scroll_to, 0, 2, Baremetal\\Slate\\Tween, 0)
    ZEND_ARG_TYPE_MASK(0, x, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_MASK(0, y, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ms, IS_LONG, 0, "300")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, ease, Baremetal\\Slate\\Ease, 0, "\\Baremetal\\Slate\\Ease::OutCubic")
ZEND_END_ARG_INFO()

/* scrollTo(): a tween of the position to (x, y), clamped by the engine; ms 0 (or a Scroll not drawn): a jump, and a
   Tween done at once. */
PHP_METHOD(SlateScroll, scrollTo)
{
    zval *zx, *zy;
    zend_long ms = 300;
    zend_object *ease = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_NUMBER(zx)
        Z_PARAM_NUMBER(zy)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
        Z_PARAM_OBJ_OF_CLASS(ease, sl_ease_ce)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    double x = zval_get_double(zx), y = zval_get_double(zy);
    if (!isfinite(x) || !isfinite(y)) {
        zend_value_error("%s::$%s must be a finite number", ZSTR_VAL(o->std.ce->name),
                         isfinite(x) ? "scrollY" : "scrollX");
        RETURN_THROWS();
    }
    if (ms < 0) {
        zend_value_error("ms must be 0 or more");
        RETURN_THROWS();
    }
    if (!sl_anim_times(ms > 0 ? ms : 1, 0, 0)) {   /* at most INT32_MAX */
        RETURN_THROWS();
    }
    sl_anim_flush();
    if (ms == 0 || !sl_realized(o)) {
        sl_scroll_sync(o);
        zval *sx = sl_slot(o, "scrollX"), *sy = sl_slot(o, "scrollY");
        zval_ptr_dtor(sx);
        ZVAL_COPY(sx, zx);
        zval_ptr_dtor(sy);
        ZVAL_COPY(sy, zy);
        if (!sl_scroll_jumped(o) || !sl_tween_instant(return_value, o)) {
            RETURN_THROWS();
        }
        return;
    }
    slate_tween_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.ms = (int32_t)ms;
    spec.ease = sl_ease_of(ease);
    spec.n = 2;
    spec.props[0].prop = SLATE_PROP_SCROLL_X;
    spec.props[0].type = SLATE_VAL_NUM;
    spec.props[0].to.num = pos_f(x);
    spec.props[1].prop = SLATE_PROP_SCROLL_Y;
    spec.props[1].type = SLATE_VAL_NUM;
    spec.props[1].to.num = pos_f(y);
    if (!sl_tween_make(return_value, o, &spec, NULL)) {
        RETURN_THROWS();
    }
    o->sc->to_tween = spec.tween;
    /* an earlier scrollTo runs on to its done (this one takes the position over) */
    cut_touch(o);   /* the scrollTo jumps the physics where it is */
}

/* ---- registration ---- */

PHP_METHOD(SlateScroll, __construct);   /* php_slate_elements.c */

#define T_NUM (MAY_BE_LONG | MAY_BE_DOUBLE)

ZEND_BEGIN_ARG_INFO_EX(arginfo_scroll_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, w, T_NUM, "1")
    ZEND_ARG_TYPE_MASK(0, h, T_NUM, "1")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, axis, Baremetal\\Slate\\ScrollAxis, 0, "\\Baremetal\\Slate\\ScrollAxis::Both")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bounce, _IS_BOOL, 0, "true")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, snapX, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, snapY, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, snapMax, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, indicators, _IS_BOOL, 0, "true")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, contentW, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, contentH, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, opacity, IS_DOUBLE, 0, "1.0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cache, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

static const zend_function_entry scroll_methods[] = {
    PHP_ME(SlateScroll, __construct, arginfo_scroll_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateScroll, touch, arginfo_scroll_touch, ZEND_ACC_PUBLIC)
    PHP_ME(SlateScroll, scrollTo, arginfo_scroll_to, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const zend_function_entry *sl_scroll_method_table(void)
{
    return scroll_methods;
}

void sl_scroll_axis_register(void)
{
    sl_scroll_axis_ce = zend_register_internal_enum("Baremetal\\Slate\\ScrollAxis", IS_UNDEF, NULL);
    for (int i = 0; i < 3; i++) {
        zend_enum_add_case_cstr(sl_scroll_axis_ce, axis_names[i], NULL);
    }
}

void sl_scroll_register(void)
{
    if (!bm_events_base_ce()) {
        return;   /* ext-events is a required module: never the case */
    }
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "ScrollEnded", NULL);
    sl_scroll_ended_ce = zend_register_internal_class_ex(&ce, bm_events_base_ce());
    sl_scroll_ended_ce->ce_flags |= ZEND_ACC_FINAL;
    zval undef;
    ZVAL_UNDEF(&undef);
    zend_declare_typed_property(sl_scroll_ended_ce, zend_string_init_interned("scroll", sizeof("scroll") - 1, 1),
                                &undef, ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                (zend_type)ZEND_TYPE_INIT_CLASS(sl_scroll_ce->name, 0, 0));
    static const char *const ints[2] = { "x", "y" };
    for (int i = 0; i < 2; i++) {
        ZVAL_UNDEF(&undef);
        zend_declare_typed_property(sl_scroll_ended_ce, zend_string_init_interned(ints[i], 1, 1), &undef,
                                    ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                    (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));
    }
}
