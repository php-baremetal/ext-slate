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
 * Touch on elements: the enum TouchType, the final class Touch, the internal event HoldDue, and the routing of the
 * touch events of the Slate's device (ext-input's Pressed, Moved, Released, Gesture) to the elements and to
 * Scene::onTouch(). Element's $touchable, $hitPad, $holdMs, on(), off() and the empty onPress()..onCancel() are
 * php_slate_elements.c's; where the touchables are on the screen is the engine's hit table (hit.h), read here without
 * a lock.
 *
 * One finger at a time (sl_touch_st). A Pressed looks the point up in the hit table, from the top: the first entry
 * whose element is alive in PHP (the id -> element map below; an entry of an element freed meanwhile, or of a scene no
 * longer shown, is passed over for the next one below) captures the touch: it gets Press, then Move and Release
 * wherever the finger goes, Tap after the Release when the finger stayed within 8 px of the Press (the distance), no
 * Hold came and the Release point is in its area now (its current entry). A Pressed that hits nothing gives the raw
 * events of that touch to Scene::onTouch(), as do the events of a touch whose Pressed Slate did not see. localX and
 * localY come from the origin of the element's current entry (not in the table now: the last one found).
 *
 * Handlers. Each kind runs the element's method (when its class overrides it) and then its callback of on(), with one
 * Touch, synchronously on the PHP task, inside the watch of the touch event (an exception stops there, as one of
 * Scene::onTouch() does). A Cancel caused inside a handler (the captured element or an ancestor removed or hidden, the
 * element no longer touchable) is queued (one entry) and delivered right after the outermost handler returns, never
 * inside it; caused outside any handler it is delivered at once. show() of another scene and the start of a
 * transition deliver it at once (Slate::show(), before onLeave()). After a Cancel the rest of the touch reaches
 * nobody.
 *
 * Hold. One persistent HAL timer, armed one-shot at a Press for the element's $holdMs (0: none) from the Pressed's
 * time, stopped at the Release, a Cancel and a move past 8 px. Its callback (the timer's task) queues a HoldDue event
 * (no data, coalescing), which Slate watches (one of ext-events' eight watches): there, when the touch is still due a
 * Hold, the element gets it; a timer that came early is armed again for what is left. A Moved or Released whose time
 * is past the due time while the Hold is still owed (the timer late, its event lost) gets the Hold first. HoldDue has
 * no listener of its own.
 *
 * Touch::$time is the touch event's $time (ext-events: ns on the base of hrtime(true), int or float as PHP integers
 * allow); a Hold's is its due time.
 *
 * Scrolls (touchable by default). A Pressed whose element on top is a Scroll gives the touch to that Scroll, driven
 * with the state machine of Scroll::touch() (php_slate_scroll.c: its threshold on its axis, its catch of a motion),
 * with the time of each touch event; no element callback. A Pressed on a Scroll that moves, or on an element inside
 * one (the chain of touchable Scroll ancestors of the hit entry, hit.h), is a catch of the innermost moving one: the
 * element under the finger gets nothing. Hand-off: when the captured element moves past 8 px before a Hold, the
 * innermost touchable Scroll among its ancestors (in the table of the same snapshot) that can move on the axis of the
 * larger movement (vertical on a tie) takes the touch: pressed at the Press point and time, its first DRAG at the
 * point past the threshold (no jump); then the element gets Cancel. None can: the element keeps the finger. The same
 * from a Scroll that has the touch, before its own drag started and unless its press was a catch, when it cannot move
 * on that axis and an ancestor can (it gets a CANCEL). A Scroll's touch ends without its Released (a release without
 * velocity, CANCEL, cmd.h) for the causes of an element's Cancel (show(), a transition, the Scroll or an ancestor
 * removed or hidden, the Scroll no longer touchable) and for a Pressed that comes before its Released; then the rest
 * of the touch reaches nobody. While the routing drives a Scroll, its touch() does nothing (no touch is fed twice).
 */
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#include "bm_events_hal.h"
#include "event_bus_php.h"
#include "hit.h"
#include "php_slate.h"

zend_class_entry *sl_touch_ce, *sl_touch_type_ce, *sl_hold_due_ce;
sl_touch_state_t sl_touch_st;

#define MOVE_THRESHOLD 8      /* px from the Press: past it, no Tap and no Hold (and a Scroll may take the touch) */
#define HOLD_EARLY_US 1000    /* a timer that comes this close to the due time counts as on time */
#define HOLD_MS_MAX 10000

static const char *const kind_names[SL_TOUCH_KINDS] = { "Press", "Move", "Release", "Tap", "Hold", "Cancel" };
static const char *const method_names[SL_TOUCH_KINDS] = { "onpress", "onmove", "onrelease", "ontap", "onhold",
                                                          "oncancel" };

/* Touch's property slots, in declaration order. */
enum { TP_TYPE, TP_ELEMENT, TP_X, TP_Y, TP_LOCAL_X, TP_LOCAL_Y, TP_TIME, TP_COUNT };
static uint32_t s_tp_offset[TP_COUNT];

/* The callbacks of on() of an element (sl_obj_t.tcb). */
struct sl_touch_cbs {
    zval fn[SL_TOUCH_KINDS];   /* a Closure, or UNDEF */
};

/* ---- the PHP task's state ---- */

static HashTable s_map;       /* element id -> sl_obj_t* (no reference) of the elements that may take a touch */
static bool s_map_ready;
static slate_hit_pub_t *s_pub;   /* the hit table of the running Slate (a reference); NULL: no touch device */
static int s_depth;           /* handlers under way (element and scene ones) */
static bool s_hold_armed;     /* the touch is due a Hold at s_hold_due */
static int64_t s_hold_due;
static bool s_watching;       /* the watch of HoldDue is set */

/* ---- shared with the timer's task ---- */

static bm_hal_timer_t *s_timer;   /* created once, never freed */
static uint16_t s_hold_tag;       /* atomic, with s_hold_on */
static bool s_hold_on;            /* a Slate with a touch device runs: the timer may queue HoldDue (atomic) */

static void hold_cb(void *arg)
{
    (void)arg;
    /* one shot. Racy with a re-arm by the PHP task for a new press in between (this stop then stops the new timer):
       accepted, it heals itself: the HoldDue emitted next reaches watch_hold(), which arms the timer again for what
       is left of the new Hold. Only when that HoldDue is dropped too (a full pool) the Hold is late, until the next
       Moved or Released delivers it (hold_if_due). */
    bm_hal_timer_stop(s_timer);
    if (__atomic_load_n(&s_hold_on, __ATOMIC_ACQUIRE)) {
        bm_events_emit(__atomic_load_n(&s_hold_tag, __ATOMIC_RELAXED), NULL);   /* dropped (a full pool): no Hold */
    }
}

static void hold_start_us(int64_t us)
{
    if (us < 1) {
        us = 1;
    }
    bm_hal_timer_stop(s_timer);
    bm_hal_timer_start_us(s_timer, (uint64_t)us);
}

static void hold_stop(void)
{
    if (s_hold_armed) {
        s_hold_armed = false;
        if (s_timer) {
            bm_hal_timer_stop(s_timer);
        }
    }
}

/* ---- elements: the map and the callbacks ---- */

void sl_touch_note(sl_obj_t *o)
{
    if (o->touch_noted) {
        return;
    }
    if (!s_map_ready) {
        zend_hash_init(&s_map, 16, NULL, NULL, 0);
        s_map_ready = true;
    }
    zend_hash_index_update_ptr(&s_map, o->id, o);
    o->touch_noted = true;
}

void sl_touch_forget(sl_obj_t *o)
{
    if (o->touch_noted && s_map_ready) {
        zend_hash_index_del(&s_map, o->id);
    }
    o->touch_noted = false;
    if (o->tcb) {
        struct sl_touch_cbs *c = o->tcb;
        o->tcb = NULL;
        for (int k = 0; k < SL_TOUCH_KINDS; k++) {
            zval_ptr_dtor(&c->fn[k]);
        }
        efree(c);
    }
}

bool sl_touch_has_cb(const sl_obj_t *o)
{
    if (!o->tcb) {
        return false;
    }
    for (int k = 0; k < SL_TOUCH_KINDS; k++) {
        if (Z_TYPE(o->tcb->fn[k]) == IS_OBJECT) {
            return true;
        }
    }
    return false;
}

void sl_touch_set_cb(sl_obj_t *o, int kind, zval *fn)
{
    if (kind < 0 || kind >= SL_TOUCH_KINDS || (!fn && !o->tcb)) {
        return;
    }
    if (!o->tcb) {
        o->tcb = ecalloc(1, sizeof *o->tcb);   /* zeroed: every zval UNDEF */
    }
    zval old;
    ZVAL_COPY_VALUE(&old, &o->tcb->fn[kind]);
    if (fn) {
        ZVAL_COPY(&o->tcb->fn[kind], fn);
    } else {
        ZVAL_UNDEF(&o->tcb->fn[kind]);
    }
    zval_ptr_dtor(&old);   /* after: it may be running (a callback that calls off() on itself is kept by the call) */
}

void sl_touch_gc(sl_obj_t *o, zend_get_gc_buffer *buf)
{
    if (!o->tcb) {
        return;
    }
    for (int k = 0; k < SL_TOUCH_KINDS; k++) {
        if (Z_TYPE(o->tcb->fn[k]) == IS_OBJECT) {
            zend_get_gc_buffer_add_zval(buf, &o->tcb->fn[k]);
        }
    }
}

int sl_touch_kind_of(zend_object *type)
{
    zval *name = zend_enum_fetch_case_name(type);
    for (int k = 0; k < SL_TOUCH_KINDS; k++) {
        if (Z_TYPE_P(name) == IS_STRING && strcmp(Z_STRVAL_P(name), kind_names[k]) == 0) {
            return k;
        }
    }
    return -1;
}

/* ---- the hit table ---- */

static bool contains(const slate_hit_t *h, int x, int y)
{
    return x >= h->x0 && x < h->x1 && y >= h->y0 && y < h->y1;
}

/* o can take a touch now, as PHP sees it: touchable (or with a callback), visible with its ancestors, in scene;
   fresh (a new touch): not at opacity 0 either, itself or an ancestor. The hit table is the last frame's: an element
   hidden, made transparent, made not touchable or removed since then is passed over. A capture under way ends for
   the same causes but opacity (fresh false). */
static bool can_take(sl_obj_t *o, zend_object *scene, bool fresh)
{
    if (!sl_slot_true(o, "touchable") && !sl_touch_has_cb(o)) {
        return false;
    }
    sl_obj_t *a = o;
    for (; a && a->base != SL_SCENE; a = a->parent) {
        if (!sl_slot_true(a, "visible") || (fresh && !(sl_slot_num(a, "opacity") > 0))) {
            return false;
        }
    }
    return a && scene && &a->std == scene;
}

/* The element on top at (x, y) among those alive in scene, *h its entry, *hi its index in the snapshot *hseq; NULL
   for none. */
static sl_obj_t *hit_object(int x, int y, zend_object *scene, slate_hit_t *h, int *hi, uint32_t *hseq)
{
    if (!s_pub || !s_map_ready) {
        return NULL;
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t seq;
        int i = slate_hit_find(s_pub, x, y, h, &seq);
        bool gone = false;
        while (i >= 0) {
            sl_obj_t *o = zend_hash_index_find_ptr(&s_map, h->id);
            if (o && can_take(o, scene, true)) {
                *hi = i;
                *hseq = seq;
                return o;
            }
            /* an element freed, or one that cannot take the touch now: the next entry below that contains the point */
            int j = i - 1;
            for (; j >= 0; j--) {
                if (!slate_hit_at(s_pub, seq, j, h)) {
                    gone = true;
                    break;
                }
                if (contains(h, x, y)) {
                    break;
                }
            }
            if (gone) {
                break;
            }
            i = j;
        }
        if (!gone) {
            return NULL;
        }
    }
    return NULL;   /* the engine kept publishing: as if nothing was hit */
}

/* The current entry of the captured element into its origin; true with *h when it has one. */
static bool find_captured(slate_hit_t *h)
{
    sl_touch_state_t *st = &sl_touch_st;
    if (!s_pub || !st->el || slate_hit_lookup(s_pub, sl_obj(st->el)->id, h, NULL) < 0) {
        return false;
    }
    st->ox = h->ox;
    st->oy = h->oy;
    return true;
}

/* ---- delivery ---- */

static void set_long(zend_object *t, int i, zend_long v)
{
    ZVAL_LONG(OBJ_PROP(t, s_tp_offset[i]), v);
}

/* Touch::$time as Event::$time (ext-events): ns on the base of hrtime(true), an int where PHP integers are 64-bit, a
   float where they are 32-bit; the same value as the $time of the touch event. */
static void set_time(zend_object *t, int64_t us)
{
    zval *slot = OBJ_PROP(t, s_tp_offset[TP_TIME]);
#if SIZEOF_ZEND_LONG == 8
    ZVAL_LONG(slot, (zend_long)(us * 1000));
#else
    ZVAL_DOUBLE(slot, (double)us * 1000.0);
#endif
}

static void flush(void);

/* The method of kind (when el's class overrides it), then its callback, with one Touch; then the queued Cancel when
   no handler is under way. Nothing with an exception pending. */
static void deliver(zend_object *el, int kind, const sl_touch_t *p, int ox, int oy)
{
    if (EG(exception)) {
        return;
    }
    zval tz;
    object_init_ex(&tz, sl_touch_ce);
    zend_object *t = Z_OBJ(tz);
    ZVAL_OBJ_COPY(OBJ_PROP(t, s_tp_offset[TP_TYPE]), zend_enum_get_case_cstr(sl_touch_type_ce, kind_names[kind]));
    ZVAL_OBJ_COPY(OBJ_PROP(t, s_tp_offset[TP_ELEMENT]), el);
    set_long(t, TP_X, p->x);
    set_long(t, TP_Y, p->y);
    set_long(t, TP_LOCAL_X, (zend_long)p->x - ox);
    set_long(t, TP_LOCAL_Y, (zend_long)p->y - oy);
    set_time(t, p->t_us);
    GC_ADDREF(el);
    s_depth++;
    zend_function *fn = zend_hash_str_find_ptr(&el->ce->function_table, method_names[kind],
                                               strlen(method_names[kind]));
    if (fn && !(fn->type == ZEND_INTERNAL_FUNCTION && fn->common.scope == sl_element_ce)) {
        zend_call_known_instance_method_with_1_params(fn, el, NULL, &tz);
    }
    sl_obj_t *o = sl_obj(el);
    if (!EG(exception) && o->tcb && Z_TYPE(o->tcb->fn[kind]) == IS_OBJECT) {
        zval cb, rv;
        ZVAL_COPY(&cb, &o->tcb->fn[kind]);   /* kept alive through the call (off() inside it) */
        ZVAL_UNDEF(&rv);
        call_user_function(NULL, NULL, &cb, &rv, 1, &tz);
        zval_ptr_dtor(&rv);
        zval_ptr_dtor(&cb);
    }
    s_depth--;
    zval_ptr_dtor(&tz);
    OBJ_RELEASE(el);
    flush();
}

/* The touch a Scroll has ends without its Released: a CANCEL to it (send; a release without velocity, no element
   callback), then the rest of the touch reaches nobody. */
static void scroll_end(bool send)
{
    sl_touch_state_t *st = &sl_touch_st;
    zend_object *s = st->el;
    st->el = NULL;
    st->owner = SL_OWN_DEAD;
    st->cancel = false;
    if (s) {
        sl_scroll_drop(sl_obj(s), send);   /* an exception (no memory for the command) is the caller's */
        OBJ_RELEASE(s);
    }
}

/* Ends the capture with a Cancel to the element: now, or after the handler under way (now: show(), a transition).
   A Scroll's touch ends at once (no handler of its own). */
static void cancel(bool now)
{
    sl_touch_state_t *st = &sl_touch_st;
    if (st->owner == SL_OWN_SCROLL) {
        scroll_end(true);
        return;
    }
    if (st->owner != SL_OWN_ELEMENT) {
        return;
    }
    hold_stop();
    if (!now && s_depth > 0) {
        st->cancel = true;
        return;
    }
    st->cancel = false;
    zend_object *el = st->el;
    st->el = NULL;
    st->owner = SL_OWN_DEAD;
    sl_touch_t p = st->last;
    deliver(el, SL_TOUCH_CANCEL, &p, st->ox, st->oy);   /* nothing with an exception pending: the capture ends */
    OBJ_RELEASE(el);
}

static void flush(void)
{
    if (s_depth == 0 && sl_touch_st.cancel) {
        cancel(true);
    }
}

void sl_touch_cancel_now(void)
{
    cancel(true);
}

void sl_touch_recheck(void)
{
    sl_touch_state_t *st = &sl_touch_st;
    if ((st->owner == SL_OWN_ELEMENT || st->owner == SL_OWN_SCROLL) && !can_take(sl_obj(st->el), sl_shown_scene(),
                                                                                  false)) {
        cancel(false);
    }
}

/* Scene::onTouch() with the raw event. */
static void scene_touch(zend_object *scene, zend_object *event)
{
    zend_function *fn = zend_hash_str_find_ptr(&scene->ce->function_table, "ontouch", sizeof("ontouch") - 1);
    if (!fn) {
        return;
    }
    GC_ADDREF(scene);   /* onTouch() may show another scene */
    zval ev;
    ZVAL_OBJ(&ev, event);
    s_depth++;
    zend_call_known_instance_method_with_1_params(fn, scene, NULL, &ev);
    s_depth--;
    OBJ_RELEASE(scene);
}

/* The Hold timer for el's $holdMs from the Press at p (none for 0). */
static void hold_arm(sl_obj_t *o, const sl_touch_t *p)
{
    hold_stop();
    double ms = sl_slot_num(o, "holdMs");
    if (!s_timer || !(ms > 0)) {
        return;
    }
    if (ms > HOLD_MS_MAX) {
        ms = HOLD_MS_MAX;   /* a subclass's default out of range */
    }
    s_hold_due = p->t_us + (int64_t)ms * 1000;
    s_hold_armed = true;
    hold_start_us(s_hold_due - bm_hal_now_us());
}

/* A Hold due by the time of p and not delivered yet (its timer late, or its HoldDue lost: a full event pool) comes
   now, before p's Move or Release; at the last point before p, with the due time. */
static void hold_if_due(const sl_touch_t *p)
{
    sl_touch_state_t *st = &sl_touch_st;
    if (!s_hold_armed || st->past || st->held || p->t_us < s_hold_due) {
        return;
    }
    hold_stop();
    st->held = true;
    slate_hit_t h;
    find_captured(&h);
    sl_touch_t q = st->last;
    q.t_us = s_hold_due;
    deliver(st->el, SL_TOUCH_HOLD, &q, st->ox, st->oy);
}

bool sl_touch_capture(zend_object *el, const sl_touch_t *t, int ox, int oy)
{
    sl_touch_state_t *st = &sl_touch_st;
    cancel(true);   /* a capture still on (its Released lost) ends first */
    GC_ADDREF(el);
    st->el = el;
    st->owner = SL_OWN_ELEMENT;
    st->taken = true;
    st->serial++;
    st->press = st->last = *t;
    st->ox = ox;
    st->oy = oy;
    st->past = st->held = st->cancel = false;
    hold_arm(sl_obj(el), t);
    deliver(el, SL_TOUCH_PRESS, t, ox, oy);
    return !EG(exception);
}

/* ---- Scrolls ---- */

/* The finger went more than 8 px (the distance) from b. */
static bool past(const sl_touch_t *a, const sl_touch_t *b)
{
    int64_t dx = (int64_t)a->x - b->x, dy = (int64_t)a->y - b->y;
    return dx * dx + dy * dy > (int64_t)MOVE_THRESHOLD * MOVE_THRESHOLD;
}


/* From entry i of the snapshot seq up the chain of touchable Scroll ancestors (hit.h, scroll), the first Scroll alive
   in scene (can_take with fresh: not at opacity 0 either, as for a new touch) that is moving (moving) or can move on
   axis (SLATE_HIT_AXIS_X / Y); NULL for none. *gone: the snapshot was replaced meanwhile (start over). */
static sl_obj_t *chain_scroll(int i, uint32_t seq, bool moving, uint8_t axis, zend_object *scene, bool *gone)
{
    slate_hit_t e;
    *gone = false;
    while (i >= 0) {
        if (!slate_hit_at(s_pub, seq, i, &e)) {
            *gone = true;
            return NULL;
        }
        if (e.flags & SLATE_HIT_SCROLL) {
            sl_obj_t *o = zend_hash_index_find_ptr(&s_map, e.id);
            if (o && o->base == SL_SCROLL && can_take(o, scene, true)
                && (moving ? sl_scroll_moving(o) : (e.flags & axis) != 0)) {
                return o;
            }
        }
        if (e.scroll >= i) {
            return NULL;   /* never: an ancestor's entry comes first */
        }
        i = e.scroll;
    }
    return NULL;
}

/* The innermost Scroll that can move on the axis of the larger movement from the Press to *t (vertical on a tie),
   from the entry of el (itself when el is a Scroll, else its touchable Scroll ancestors); NULL for none (or el not
   in the table now). */
static sl_obj_t *taker(sl_obj_t *el, const sl_touch_t *t)
{
    sl_touch_state_t *st = &sl_touch_st;
    int dx = t->x - st->press.x, dy = t->y - st->press.y;
    uint8_t axis = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? SLATE_HIT_AXIS_X : SLATE_HIT_AXIS_Y;
    zend_object *scene = sl_shown_scene();
    for (int attempt = 0; attempt < 8 && s_pub; attempt++) {
        slate_hit_t h;
        uint32_t seq;
        int i = slate_hit_lookup(s_pub, el->id, &h, &seq);
        if (i < 0) {
            return NULL;
        }
        bool gone;
        sl_obj_t *o = chain_scroll(el->base == SL_SCROLL ? i : h.scroll, seq, false, axis, scene, &gone);
        if (!gone) {
            return o;
        }
    }
    return NULL;
}

/* The touch goes to the Scroll s: pressed at *press, then (drag not NULL) its first DRAG at *drag (a hand-off). The
   owner is the Scroll before anything else runs. false with an exception. */
static bool scroll_take(sl_obj_t *s, const sl_touch_t *press, const sl_touch_t *drag)
{
    sl_touch_state_t *st = &sl_touch_st;
    hold_stop();
    GC_ADDREF(&s->std);
    st->el = &s->std;
    st->owner = SL_OWN_SCROLL;
    st->taken = true;
    st->serial++;
    st->press = *press;
    st->last = drag ? *drag : *press;
    st->past = drag != NULL;
    st->held = st->cancel = false;
    bool ok = drag ? sl_scroll_take_drag(s, press, drag) : sl_scroll_take(s, press);
    if (!ok && st->owner == SL_OWN_SCROLL && st->el == &s->std) {
        /* the PRESS refused: nothing went, nothing is sent; the hand-off's first DRAG refused after its PRESS went: a
           CANCEL, so the engine is not left pressed (best effort: with no memory it may be refused too, chained to the
           first exception; the PHP side ends the touch anyway) */
        scroll_end(true);
    }
    return ok;
}

/* A Moved or the Released of a touch a Scroll has. Past 8 px from the Press (the first time), before the drag of
   its own started or a catch: when it cannot move on that axis and a Scroll among its ancestors can, the innermost
   such takes the touch over (the first one gets a CANCEL). */
static void scroll_event(const sl_touch_t *p, bool released)
{
    sl_touch_state_t *st = &sl_touch_st;
    sl_obj_t *s = sl_obj(st->el);
    st->last = *p;
    if (!released && !st->past && past(p, &st->press)) {
        st->past = true;
        if (!sl_scroll_owns(s)) {
            sl_obj_t *t = taker(s, p);
            if (t && t != s) {
                sl_touch_t press = st->press;
                scroll_end(true);
                if (!EG(exception)) {
                    scroll_take(t, &press, p);
                }
                return;
            }
        }
    }
    sl_scroll_event(s, p, released);   /* an exception stops there, as one of a handler does */
    if (released) {
        zend_object *o = st->el;
        st->el = NULL;
        st->owner = SL_OWN_NONE;
        OBJ_RELEASE(o);
    }
}

/* ---- routing ---- */

static zend_class_entry *s_ev_ce[4];   /* Pressed, Moved, Released, Gesture of ext-input */

static int event_kind(zend_class_entry *ce)
{
    static const char *const names[4] = {
        "baremetal\\input\\pressed", "baremetal\\input\\moved", "baremetal\\input\\released",
        "baremetal\\input\\gesture",
    };
    for (int i = 0; i < 4; i++) {
        if (!s_ev_ce[i]) {
            s_ev_ce[i] = zend_hash_str_find_ptr(CG(class_table), names[i], strlen(names[i]));
        }
        if (s_ev_ce[i] == ce) {
            return i;
        }
    }
    return -1;
}

static void event_point(zend_object *ev, sl_touch_t *p)
{
    zval rv;
    zval *v = zend_read_property(ev->ce, ev, "x", 1, 1, &rv);
    p->x = v ? (int)zval_get_long(v) : 0;
    v = zend_read_property(ev->ce, ev, "y", 1, 1, &rv);
    p->y = v ? (int)zval_get_long(v) : 0;
    v = zend_read_property(ev->ce, ev, "time", 4, 1, &rv);   /* ns on the base of hrtime(true): µs * 1000 */
    if (v && Z_TYPE_P(v) == IS_LONG) {
        p->t_us = (int64_t)Z_LVAL_P(v) / 1000;
    } else if (v && Z_TYPE_P(v) == IS_DOUBLE) {
        p->t_us = (int64_t)(Z_DVAL_P(v) / 1000.0);
    } else {
        p->t_us = bm_hal_now_us();
    }
}

void sl_touch_unseen(zend_object *event)
{
    sl_touch_state_t *st = &sl_touch_st;
    if (event_kind(event->ce) == 0 && st->owner != SL_OWN_ELEMENT && st->owner != SL_OWN_SCROLL) {
        st->owner = SL_OWN_NONE;   /* never ELEMENT or SCROLL during a transition: its start cancelled them */
        st->taken = false;
    }
}

void sl_touch_route(zend_object *event, zend_object *scene)
{
    sl_touch_state_t *st = &sl_touch_st;
    int k = event_kind(event->ce);
    if (k < 0) {
        return;
    }
    sl_touch_t p;
    if (k == 3) {
        /* Gesture: the scene's, unless an element or a Scroll has the touch or had it (the CST816S sends it after the
           Released), or it was cancelled */
        if (st->owner == SL_OWN_SCENE || (st->owner == SL_OWN_NONE && !st->taken)) {
            scene_touch(scene, event);
        }
        return;
    }
    event_point(event, &p);
    if (k == 0) {   /* Pressed */
        cancel(true);   /* a capture whose Released was lost */
        st->owner = SL_OWN_NONE;
        st->taken = false;
        slate_hit_t h;
        int hi = -1;
        uint32_t seq = 0;
        sl_obj_t *o = hit_object(p.x, p.y, scene, &h, &hi, &seq);
        /* a Scroll moving under the finger (itself or one it is in) is caught: the touch is that Scroll's, the element
           under it gets nothing; else a Scroll hit takes the touch, any other element captures it. The snapshot
           replaced between the lookup and the walk: both again (at most 8 times, then as nothing was moving) */
        sl_obj_t *s = NULL;
        for (int attempt = 0; o && attempt < 8; attempt++) {
            bool gone;
            s = chain_scroll(hi, seq, true, 0, scene, &gone);
            if (!gone) {
                break;
            }
            s = NULL;
            o = hit_object(p.x, p.y, scene, &h, &hi, &seq);
        }
        if (o) {
            if (!s && o->base == SL_SCROLL) {
                s = o;
            }
            if (s) {
                scroll_take(s, &p, NULL);
                return;
            }
            sl_touch_capture(&o->std, &p, h.ox, h.oy);
            return;
        }
        st->owner = SL_OWN_SCENE;
        scene_touch(scene, event);
        return;
    }
    bool released = k == 2;
    switch (st->owner) {
    case SL_OWN_ELEMENT: {
        uint32_t serial = st->serial;
        hold_if_due(&p);
        if (st->owner != SL_OWN_ELEMENT || st->serial != serial) {
            if (released && st->owner == SL_OWN_DEAD) {
                st->owner = SL_OWN_NONE;   /* cancelled in onHold(): the touch is over */
            }
            return;
        }
        slate_hit_t h;
        st->last = p;
        bool found = find_captured(&h);
        if (!st->past && past(&p, &st->press)) {
            st->past = true;
            hold_stop();
            sl_obj_t *s = released || st->held ? NULL : taker(sl_obj(st->el), &p);
            if (s) {
                /* the hand-off: the Scroll has the touch (pressed where and when the element was, its first DRAG
                   here), then the element gets Cancel; what that Cancel does (a show(), a removal) acts on the
                   Scroll's touch */
                zend_object *el = st->el;
                int ox = st->ox, oy = st->oy;
                st->el = NULL;
                sl_touch_t press = st->press;
                scroll_take(s, &press, &p);
                deliver(el, SL_TOUCH_CANCEL, &p, ox, oy);
                OBJ_RELEASE(el);
                return;
            }
        }
        if (!released) {
            deliver(st->el, SL_TOUCH_MOVE, &p, st->ox, st->oy);
            return;
        }
        hold_stop();
        bool tap = !st->past && !st->held && found && contains(&h, p.x, p.y);
        zend_object *el = st->el;
        int ox = st->ox, oy = st->oy;
        st->el = NULL;   /* the touch is over: a cause of Cancel in onRelease() or onTap() changes nothing */
        st->owner = SL_OWN_NONE;
        deliver(el, SL_TOUCH_RELEASE, &p, ox, oy);
        if (tap) {
            deliver(el, SL_TOUCH_TAP, &p, ox, oy);
        }
        OBJ_RELEASE(el);
        return;
    }
    case SL_OWN_SCROLL:
        scroll_event(&p, released);
        return;
    case SL_OWN_DEAD:
        break;
    default:
        scene_touch(scene, event);
        break;
    }
    if (released) {
        st->owner = SL_OWN_NONE;
    }
}

/* Slate's watch of HoldDue: the Hold of the touch, when it is still due one. */
static void watch_hold(zend_object *event, void *arg)
{
    (void)event;
    (void)arg;
    sl_touch_state_t *st = &sl_touch_st;
    if (!s_hold_armed || st->owner != SL_OWN_ELEMENT) {
        return;   /* stopped meanwhile, or a HoldDue dispatched by a script */
    }
    int64_t left = s_hold_due - bm_hal_now_us();
    if (left > HOLD_EARLY_US) {
        hold_start_us(left);   /* early: armed again for what is left */
        return;
    }
    s_hold_armed = false;
    if (st->past || st->held) {
        return;
    }
    st->held = true;
    slate_hit_t h;
    find_captured(&h);
    sl_touch_t p = st->last;
    p.t_us = s_hold_due;
    deliver(st->el, SL_TOUCH_HOLD, &p, st->ox, st->oy);
}

/* ---- the Slate ---- */

bool sl_touch_start(slate_engine_t *e)
{
    if (!s_pub) {
        s_pub = slate_hit_pub_new();
        if (!s_pub) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: out of memory for the touch", 0);
            return false;
        }
    }
    slate_engine_set_hit_pub(e, s_pub);
    if (!s_timer) {
        s_timer = bm_hal_timer_create(hold_cb, NULL, "slate-hold");
        if (!s_timer) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: out of memory for the touch", 0);
            return false;
        }
    }
    uint16_t tag;
    if (!bm_events_source_tag_ex(sl_hold_due_ce->name, true, &tag)) {   /* coalescing: one waits at most */
        if (!EG(exception)) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Slate: cannot raise %s: too many event sources",
                                    ZSTR_VAL(sl_hold_due_ce->name));
        }
        return false;
    }
    if (!bm_events_watch(sl_hold_due_ce, watch_hold, NULL)) {
        zend_throw_exception(spl_ce_RuntimeException,
                             "Slate: cannot take the touch: the table of ext-events' watches is full", 0);
        return false;
    }
    s_watching = true;
    __atomic_store_n(&s_hold_tag, tag, __ATOMIC_RELAXED);
    __atomic_store_n(&s_hold_on, true, __ATOMIC_RELEASE);
    return true;
}

void sl_touch_stop(slate_engine_t *e)
{
    sl_touch_state_t *st = &sl_touch_st;
    hold_stop();
    __atomic_store_n(&s_hold_on, false, __ATOMIC_RELEASE);
    if (s_watching) {
        bm_events_unwatch(sl_hold_due_ce, watch_hold, NULL);
        s_watching = false;
    }
    zend_object *el = st->el;
    if (el && st->owner == SL_OWN_SCROLL) {
        sl_scroll_drop(sl_obj(el), false);   /* the engine stops with it */
    }
    memset(st, 0, sizeof *st);   /* the touch goes without a Cancel: the Slate is gone */
    if (el) {
        OBJ_RELEASE(el);
    }
    if (s_pub) {
        if (e) {
            slate_engine_set_hit_pub(e, NULL);
        }
        slate_hit_pub_put(s_pub);   /* the engine gives its own back at its stop */
        s_pub = NULL;
    }
}

void sl_touch_rshutdown(void)
{
    if (s_map_ready) {
        zend_hash_destroy(&s_map);
        s_map_ready = false;
    }
    s_depth = 0;
}

/* ---- registration ---- */

ZEND_BEGIN_ARG_INFO_EX(arginfo_touch_ctor, 0, 0, 0)
ZEND_END_ARG_INFO()

/* Private: a Touch is made by Slate only (object_init_ex, no constructor call). */
PHP_METHOD(SlateTouch, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

static const zend_function_entry touch_methods[] = {
    PHP_ME(SlateTouch, __construct, arginfo_touch_ctor, ZEND_ACC_PRIVATE)
    PHP_FE_END
};

void sl_touch_register(void)
{
    sl_touch_type_ce = zend_register_internal_enum("Baremetal\\Slate\\TouchType", IS_UNDEF, NULL);
    for (int k = 0; k < SL_TOUCH_KINDS; k++) {
        zend_enum_add_case_cstr(sl_touch_type_ce, kind_names[k], NULL);
    }

    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Touch", touch_methods);
    sl_touch_ce = zend_register_internal_class(&ce);
    sl_touch_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    static const char *const names[TP_COUNT] = { "type", "element", "x", "y", "localX", "localY", "time" };
    zend_string *type_name = zend_string_init_interned("Baremetal\\Slate\\TouchType",
                                                       sizeof("Baremetal\\Slate\\TouchType") - 1, 1);
    zend_string *el_name = zend_string_init_interned("Baremetal\\Slate\\Element",
                                                     sizeof("Baremetal\\Slate\\Element") - 1, 1);
    for (int i = 0; i < TP_COUNT; i++) {
        zval undef;
        ZVAL_UNDEF(&undef);
        zend_type t = i == TP_TYPE ? (zend_type)ZEND_TYPE_INIT_CLASS(type_name, 0, 0)
                      : i == TP_ELEMENT ? (zend_type)ZEND_TYPE_INIT_CLASS(el_name, 0, 0)
                      : i == TP_TIME ? (zend_type)ZEND_TYPE_INIT_MASK(MAY_BE_LONG | MAY_BE_DOUBLE)
                                     : (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0);
        zend_property_info *pi = zend_declare_typed_property(
            sl_touch_ce, zend_string_init_interned(names[i], strlen(names[i]), 1), &undef,
            ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL, t);
        s_tp_offset[i] = pi->offset;
    }

    if (bm_events_base_ce()) {   /* ext-events is a required module: always */
        INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "HoldDue", NULL);
        sl_hold_due_ce = zend_register_internal_class_ex(&ce, bm_events_base_ce());
        sl_hold_due_ce->ce_flags |= ZEND_ACC_FINAL;
    }
}
