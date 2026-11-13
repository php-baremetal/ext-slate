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
 * Animations in PHP: the Tween (its $done, stop(), wait(), then()), the event TweenDone and the enum Ease; the page
 * transitions of Slate::show() (Transition, Direction, TransitionDone: see "transitions" below).
 * Element::animate() and Element::ease() are in php_slate_elements.c (they check and write the properties); the
 * tweens themselves run in the engine (engine.h, Tweens).
 *
 * A Tween has an id, unique in the request, that its TWEEN command carries. While it runs it holds a reference on
 * its element, and a registry holds a reference on it (so a Tween nobody keeps still gets its TweenDone).
 *
 * Dones. When tweens end by themselves the engine queues their ids and calls our listener on its task, once per
 * queue that was empty. The listener (it never calls PHP) drains the queue: each id goes into a list of finished
 * tweens and gets a TweenDone event of ext-events whose 4 bytes of data are the id, and a wait() is woken. On the
 * PHP task the event's filler takes the id out of the list, marks the Tween done (its $done, its element released)
 * and fills the event. wait() drains the engine's queue the same way and reads the list without taking from it, so
 * the event still follows. As for the images (php_slate_images.c), an event that is never delivered (the pool was
 * full, or init-loop gave it back unread) leaves its id in the list: a hook run when the PHP task returns to the
 * event loop marks those tweens done (their TweenDone is lost, their $done is not).
 *
 * Pending. A tween of an element not realized yet (made in build(), or before add()), or chained after a pending
 * one, keeps its spec and is sent when the element is realized (sl_anim_realized), the values before animate()
 * first. Like a tween of a scene that is not shown, it waits.
 *
 * Stops. A tween that the engine stops never ends: stop() (with the chain of then() after it), a remove() of its
 * element (or the free of its group) that the delivery does not undo (sl_deliver checks the elements detached since
 * the last one, and sends TWEEN_STOP: a FREE of the parent does not stop them in the engine), the Slate
 * stopping. These are marked stopped here at once: $done stays false and wait() returns false without waiting.
 *
 * Releases from plain C (the idle hook, a filler) could run a destructor of a user element; the elements released
 * there wait in a list until the next call of the Slate API or property write (sl_anim_flush), the Slate's stop or
 * the end of the
 * request.
 */
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#include "bm_events_hal.h"
#include "event_bus_php.h"
#include "php_slate.h"

zend_class_entry *sl_tween_ce, *sl_tween_done_ce, *sl_ease_ce;

static zend_object_handlers tween_handlers;
static uint32_t s_off_done;

enum { TW_RUNNING, TW_DONE, TW_STOPPED, TW_PENDING };

/* Alive: running in the engine, or pending (its element not drawn yet, or the tween it follows still pending). */
#define ALIVE(t) ((t)->state == TW_RUNNING || (t)->state == TW_PENDING)

typedef struct sl_tween sl_tween_t;
struct sl_tween {
    uint32_t id;
    uint8_t state;           /* TW_*; a new Tween is TW_STOPPED until sl_tween_make sets it */
    bool endless;            /* repeat -1 */
    bool held;               /* the registry's reference: while it runs, then until its TweenDone is filled or lost */
    bool listed;             /* in s_all */
    zend_object *el;         /* its element (a reference) while it is alive */
    sl_tween_t *prev, *next; /* the chain of then(), among alive tweens */
    slate_tween_spec_t *spec;/* TW_PENDING: what to send when its element is realized (emalloc; props' `from` are the
                                values before animate(), set first) */
    uint32_t props;          /* its properties (bit 1u << slate_prop_t), as made (a pending one: as left by writes) */
    zend_object std;
};

/* The masks of tweens (props) and ease() hold the animatable properties, the last of which is a scroll's SCROLL_Y
   (scrollTo()); CONTENT_W, CONTENT_H and INDICATOR_A are never animated from PHP and stay out of them. */
_Static_assert(SLATE_PROP_SCROLL_Y < 32, "the animatable properties fit a 32-bit mask");

static sl_tween_t *tween_of(zend_object *o)
{
    return (sl_tween_t *)((char *)o - XtOffsetOf(sl_tween_t, std));
}

/* ---- the shared state (engine task and PHP task) ---- */

typedef struct sl_tdone {
    uint32_t id;
    struct sl_tdone *next;
} sl_tdone_t;

static bm_hal_lock_t *s_lock;     /* the list, s_emitting, s_accepting; created once, never freed */
static bm_hal_queue_t *s_wake;    /* a token when dones were drained, for wait(); created once */
static sl_tdone_t *s_head, *s_tail;
static int s_emitting;            /* drains between an append and the end of its emit */
static bool s_accepting;          /* a request runs with the event set up */
static uint16_t s_tag;
static slate_engine_t *s_engine;  /* the engine the listener drains (atomic) */

/* ---- the PHP task's state (request memory) ---- */

static bool s_ready;
static HashTable s_all;           /* id -> sl_tween_t* of every Tween object alive (no reference) */
static HashTable s_removed;       /* handle -> zend_object* (a reference): removed elements, checked at the delivery */
static zend_object **s_trash;     /* references to release from PHP (emalloc) */
static uint32_t s_trash_n, s_trash_cap;
static uint32_t s_next_id;

static bm_hal_lock_t *lock(void)
{
    bm_hal_lock_t *l = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
    bm_hal_lock_enter(l);
    return l;
}

/* ---- dones (any task) ---- */

/* Queues the id and its TweenDone. Any task (not an ISR). */
static void queue_done(uint32_t id)
{
    sl_tdone_t *n = bm_hal_alloc(sizeof *n, BM_HAL_MEM_EXTERNAL);
    if (!n) {
        return;   /* no memory even for this: the Tween keeps running for PHP (wait() times out) */
    }
    n->id = id;
    n->next = NULL;
    bm_hal_lock_t *l = lock();
    if (!s_accepting) {
        bm_hal_lock_exit(l);
        bm_hal_free(n);
        return;
    }
    if (s_tail) {
        s_tail->next = n;
    } else {
        s_head = n;
    }
    s_tail = n;
    s_emitting++;
    uint16_t tag = s_tag;
    bm_hal_lock_exit(l);
    bm_events_emit_data(tag, NULL, &id, sizeof id);   /* dropped: the idle hook applies it */
    l = lock();
    s_emitting--;
    bm_hal_lock_exit(l);
}

/* Every id the engine has queued, into the list. Any task: the engine's listener, wait(). */
static void pull(slate_engine_t *e)
{
    if (!e || !__atomic_load_n(&s_lock, __ATOMIC_ACQUIRE)) {
        return;
    }
    uint32_t id;
    bool any = false;
    while (slate_engine_tween_done(e, &id)) {
        queue_done(id);
        any = true;
    }
    if (any) {
        uint8_t t = 0;
        bm_hal_queue_send(s_wake, &t, 0);
    }
}

static void pull_tr(slate_engine_t *e);

/* The engine's tween listener: its task, outside its locks. It is also the transitions' and the scrolls' listener:
   the three queues are drained (the engine calls it only when one of them was empty). */
static void on_tween_done(void *arg)
{
    (void)arg;
    slate_engine_t *e = __atomic_load_n(&s_engine, __ATOMIC_ACQUIRE);
    pull(e);
    pull_tr(e);
    sl_scroll_pull(e);
}

/* Takes the id's entry out of the list (NULL: not there). */
static sl_tdone_t *take(uint32_t id)
{
    bm_hal_lock_t *l = lock();
    sl_tdone_t *prev = NULL, *n = s_head;
    while (n && n->id != id) {
        prev = n;
        n = n->next;
    }
    if (n) {
        if (prev) {
            prev->next = n->next;
        } else {
            s_head = n->next;
        }
        if (s_tail == n) {
            s_tail = prev;
        }
    }
    bm_hal_lock_exit(l);
    return n;
}

static bool listed(uint32_t id)
{
    bm_hal_lock_t *l = lock();
    sl_tdone_t *n = s_head;
    while (n && n->id != id) {
        n = n->next;
    }
    bm_hal_lock_exit(l);
    return n != NULL;
}

/* ---- the Tweens (PHP task) ---- */

static void trash(zend_object *o)
{
    if (s_trash_n == s_trash_cap) {
        s_trash_cap = s_trash_cap ? s_trash_cap * 2 : 16;
        s_trash = erealloc(s_trash, s_trash_cap * sizeof *s_trash);
    }
    s_trash[s_trash_n++] = o;
}

void sl_anim_flush(void)
{
    while (s_trash_n > 0) {   /* a release may trash more */
        zend_object *o = s_trash[--s_trash_n];
        OBJ_RELEASE(o);
    }
}

static sl_tween_t *find(uint32_t id)
{
    return s_ready ? zend_hash_index_find_ptr(&s_all, id) : NULL;
}

/* t no longer runs: out of its chain, its element given back (trashed: plain C may be the caller). */
static void unrun(sl_tween_t *t)
{
    if (t->next) {
        t->next->prev = NULL;
        t->next = NULL;
    }
    if (t->prev) {
        t->prev->next = NULL;
        t->prev = NULL;
    }
    if (t->el) {
        sl_obj_t *el = sl_obj(t->el);
        if (el->tweens > 0) {
            el->tweens--;
        }
        trash(t->el);
        t->el = NULL;
    }
    if (t->spec) {
        efree(t->spec);
        t->spec = NULL;
    }
}

/* The registry's reference goes. t has no element by now, so its release runs no PHP. */
static void unhold(sl_tween_t *t)
{
    if (t->held) {
        t->held = false;
        OBJ_RELEASE(&t->std);
    }
}

/* t ended by itself (the engine says so: also after PHP marked it stopped, when it ended before the stop). */
static void finish(sl_tween_t *t)
{
    if (t->state == TW_DONE) {
        return;
    }
    t->state = TW_DONE;
    ZVAL_TRUE(OBJ_PROP(&t->std, s_off_done));
    unrun(t);
}

/* t and the tweens chained after it are stopped (the engine stops them without a done). */
static void stop_chain(sl_tween_t *t)
{
    if (t->prev) {
        t->prev->next = NULL;
        t->prev = NULL;
    }
    while (t) {
        sl_tween_t *nx = t->next;
        if (ALIVE(t)) {
            t->state = TW_STOPPED;
            unrun(t);
            unhold(t);   /* may free t */
        }
        t = nx;
    }
}

/* Stops the alive tweens of el (with their chains); el NULL: every running one (and their chains). cmd: a
   TWEEN_STOP goes to the engine for each running one (no exception: plain C may be the caller). */
static void stop_where(zend_object *el, bool cmd)
{
    uint32_t n = 0, cap = 0, *ids = NULL;
    sl_tween_t *t;
    ZEND_HASH_FOREACH_PTR(&s_all, t) {
        if (el ? ALIVE(t) && t->el == el : t->state == TW_RUNNING) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                ids = erealloc(ids, cap * sizeof *ids);
            }
            ids[n++] = t->id;
        }
    } ZEND_HASH_FOREACH_END();
    for (uint32_t i = 0; i < n; i++) {   /* by id: a stop frees Tweens, which leave s_all */
        sl_tween_t *x = find(ids[i]);
        if (x && ALIVE(x)) {
            if (cmd && x->state == TW_RUNNING && sl_g.engine) {
                slate_cmd_t c = { .kind = SLATE_CMD_TWEEN_STOP, .id = x->id, .v = slate_v_bool(false) };
                slate_cmd_push(slate_engine_cmds(sl_g.engine), &c);
            }
            stop_chain(x);
        }
    }
    if (ids) {
        efree(ids);
    }
}

/* Back in the event loop (plain C, from Slate's idle hook): the dones whose events are gone undelivered. */
void sl_anim_idle(void)
{
    if (!s_ready) {
        return;
    }
    bm_hal_lock_t *l = lock();
    sl_tdone_t *last = s_emitting == 0 ? s_tail : NULL;
    bm_hal_lock_exit(l);
    if (!last) {
        return;
    }
    evt_tag_stats_t st;
    evt_tag_stats(s_tag, &st);
    if (st.queued != 0) {
        return;
    }
    /* every done up to last had its event emitted, and none is queued: they were given back or dropped */
    l = lock();
    sl_tdone_t *list = s_head;
    s_head = last->next;
    if (!s_head) {
        s_tail = NULL;
    }
    last->next = NULL;
    bm_hal_lock_exit(l);
    while (list) {
        sl_tdone_t *next = list->next;
        sl_tween_t *t = find(list->id);
        if (t) {
            finish(t);
            unhold(t);
        }
        bm_hal_free(list);
        list = next;
    }
}

/* The filler of TweenDone (PHP task, before the delivery). */
static void fill_done(zend_object *ev, zend_object *dev, const evt_t *slot)
{
    (void)dev;
    size_t len;
    const void *d = bm_events_slot_data(slot, &len);
    uint32_t id = 0;
    if (d && len == sizeof id) {
        memcpy(&id, d, sizeof id);
    }
    sl_tdone_t *n = s_ready ? take(id) : NULL;
    sl_tween_t *t = find(id);
    zval zv;
    if (t) {
        finish(t);
        ZVAL_OBJ(&zv, &t->std);
        bm_events_set_zval(ev, sl_tween_done_ce, "tween", &zv);   /* the event's own reference */
        if (n) {
            unhold(t);
        }
    } else {
        /* a Tween stopped and freed in PHP that the engine had ended just before the stop: a stand-in, done */
        object_init_ex(&zv, sl_tween_ce);
        sl_tween_t *x = tween_of(Z_OBJ(zv));
        x->id = id;
        x->state = TW_DONE;
        ZVAL_TRUE(OBJ_PROP(&x->std, s_off_done));
        bm_events_set_zval(ev, sl_tween_done_ce, "tween", &zv);
        zval_ptr_dtor(&zv);
    }
    if (n) {
        bm_hal_free(n);
    }
}

/* The lock and the wake queue, created once (never freed). false with an exception. */
static bool lock_ready(void)
{
    if (s_lock) {
        return true;
    }
    bm_hal_lock_t *l = bm_hal_lock_create();
    bm_hal_queue_t *q = l ? bm_hal_queue_create(1, 1) : NULL;
    if (!q) {
        zend_throw_exception(spl_ce_RuntimeException, "Slate: no memory for the animations", 0);
        return false;   /* the lock (if any) is kept: never freed */
    }
    s_wake = q;
    __atomic_store_n(&s_lock, l, __ATOMIC_RELEASE);
    return true;
}

/* The first Tween of the request: the lock and the wake queue (once), the event class as a source of ext-events,
   the registries, the idle hook. false with an exception. */
static bool ready(void)
{
    if (s_ready) {
        return true;
    }
    if (!lock_ready()) {
        return false;
    }
    uint16_t tag;
    if (!bm_events_source_tag(sl_tween_done_ce->name, &tag)) {
        if (!EG(exception)) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Slate: cannot raise %s: too many event sources",
                                    ZSTR_VAL(sl_tween_done_ce->name));
        }
        return false;
    }
    bm_events_set_filler(tag, fill_done);
    if (!sl_idle_use(SL_IDLE_ANIM, true)) {
        /* ext-events keeps 4 hooks; Slate takes 1 for the running Slate, the images and the animations */
        bm_events_set_filler(tag, NULL);
        zend_throw_exception(spl_ce_RuntimeException,
                             "Slate: cannot animate: the table of ext-events' idle hooks is full", 0);
        return false;
    }
    zend_hash_init(&s_all, 16, NULL, NULL, 0);
    zend_hash_init(&s_removed, 4, NULL, NULL, 0);
    bm_hal_lock_t *l = lock();
    s_tag = tag;
    s_accepting = true;
    bm_hal_lock_exit(l);
    s_ready = true;
    return true;
}

static void trans_stopped(void);
static void trans_rshutdown(void);

static void trash_free(void)
{
    sl_anim_flush();
    if (s_trash) {
        efree(s_trash);
        s_trash = NULL;
        s_trash_cap = 0;
    }
}

void sl_anim_rshutdown(void)
{
    trans_rshutdown();
    if (!s_ready) {
        trash_free();
        return;
    }
    sl_idle_use(SL_IDLE_ANIM, false);
    bm_hal_lock_t *l = lock();
    s_accepting = false;
    sl_tdone_t *list = s_head;
    s_head = s_tail = NULL;
    bm_hal_lock_exit(l);
    while (list) {
        sl_tdone_t *next = list->next;
        bm_hal_free(list);
        list = next;
    }
    bm_events_set_filler(s_tag, NULL);
    /* the registry's references and the removed elements go; the Tweens freed from now on are not listed */
    uint32_t n = 0;
    sl_tween_t **held = zend_hash_num_elements(&s_all) ? emalloc(zend_hash_num_elements(&s_all) * sizeof *held) : NULL;
    sl_tween_t *t;
    ZEND_HASH_FOREACH_PTR(&s_all, t) {
        t->listed = false;
        if (t->held) {
            held[n++] = t;
        }
    } ZEND_HASH_FOREACH_END();
    zend_object *o;
    ZEND_HASH_FOREACH_PTR(&s_removed, o) {
        trash(o);
    } ZEND_HASH_FOREACH_END();
    zend_hash_destroy(&s_all);
    zend_hash_destroy(&s_removed);
    s_ready = false;
    for (uint32_t i = 0; i < n; i++) {
        if (ALIVE(held[i])) {
            held[i]->state = TW_STOPPED;
        }
        if (held[i]->spec) {
            efree(held[i]->spec);
            held[i]->spec = NULL;
        }
        held[i]->prev = held[i]->next = NULL;
        if (held[i]->el) {
            trash(held[i]->el);
            held[i]->el = NULL;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        unhold(held[i]);
    }
    if (held) {
        efree(held);
    }
    trash_free();
}

void sl_anim_engine(slate_engine_t *e)
{
    __atomic_store_n(&s_engine, e, __ATOMIC_RELEASE);
    if (!e) {
        trans_stopped();
    }
    if (!e && s_ready) {
        /* the engine and its tweens are gone: none of them will end (pending ones wait for the next Slate) */
        stop_where(NULL, false);
        zend_object *o;
        ZEND_HASH_FOREACH_PTR(&s_removed, o) {
            trash(o);
        } ZEND_HASH_FOREACH_END();
        zend_hash_clean(&s_removed);
    }
}

void sl_anim_removed(sl_obj_t *c)
{
    if (!s_ready) {
        return;
    }
    if (!sl_g.engine) {
        stop_where(&c->std, false);   /* no delivery to wait for (and no move to keep) */
        return;
    }
    if (!zend_hash_index_exists(&s_removed, c->std.handle)) {
        GC_ADDREF(&c->std);
        zend_hash_index_add_new_ptr(&s_removed, c->std.handle, &c->std);
    }
}

void sl_deliver(void)
{
    if (s_ready && zend_hash_num_elements(&s_removed) > 0) {
        zend_object *o;
        ZEND_HASH_FOREACH_PTR(&s_removed, o) {
            sl_obj_t *c = sl_obj(o);
            /* REMOVE then ADD in one delivery is a move: the engine keeps the tweens of an element that has a
               parent again (an ADD is sent only into a realized group) */
            if (c->tweens && !(c->parent && (sl_realized(c->parent) || !sl_realized(c)))) {
                /* a REMOVE stops them in the engine already; a FREE of the parent does not: TWEEN_STOP */
                stop_where(o, true);
            }
            trash(o);
        } ZEND_HASH_FOREACH_END();
        zend_hash_clean(&s_removed);
    }
    if (sl_g.engine) {
        slate_engine_deliver(sl_g.engine);
    }
}

bool sl_anim_times(zend_long ms, zend_long delay, zend_long repeat)
{
    if (ms < 1) {
        zend_value_error("ms must be at least 1");
        return false;
    }
    if (delay < 0) {
        zend_value_error("delay must be 0 or more");
        return false;
    }
    if (repeat < -1) {
        zend_value_error("repeat must be -1 or more");
        return false;
    }
#if SIZEOF_ZEND_LONG > 4   /* a 32-bit zend_long is never past INT32_MAX (and the compare would be always false) */
    if (ms > INT32_MAX || delay > INT32_MAX || repeat > INT32_MAX) {
        zend_value_error("%s must be at most %d", ms > INT32_MAX ? "ms" : delay > INT32_MAX ? "delay" : "repeat",
                         (int)INT32_MAX);
        return false;
    }
#endif
    return true;
}

static const char *const ease_names[SLATE_EASE_COUNT] = {
    "Linear", "InQuad", "OutQuad", "InOutQuad", "InCubic", "OutCubic", "InOutCubic", "OutBack", "OutBounce",
    "OutElastic",
};

uint8_t sl_ease_of(zend_object *ease)
{
    if (ease) {
        zval *name = zend_enum_fetch_case_name(ease);
        for (int i = 0; i < SLATE_EASE_COUNT; i++) {
            if (Z_TYPE_P(name) == IS_STRING && strcmp(Z_STRVAL_P(name), ease_names[i]) == 0) {
                return (uint8_t)i;
            }
        }
    }
    return SLATE_EASE_OUT_CUBIC;
}

bool sl_tween_make(zval *rv, sl_obj_t *el, slate_tween_spec_t *spec, zend_object *pred)
{
    if (!ready()) {
        return false;
    }
    sl_anim_flush();
    object_init_ex(rv, sl_tween_ce);
    sl_tween_t *t = tween_of(Z_OBJ_P(rv));
    if (++s_next_id == 0) {
        s_next_id = 1;
    }
    t->id = s_next_id;
    t->endless = spec->repeat < 0;
    zend_hash_index_update_ptr(&s_all, t->id, t);
    t->listed = true;
    spec->tween = t->id;
    sl_tween_t *p = pred ? tween_of(pred) : NULL;
    if (p && !ALIVE(p)) {
        p = NULL;   /* after a tween that ended or stopped: at once */
    }
    while (p && p->next) {
        p = p->next;   /* it follows the end of the chain (as the engine appends): that one decides */
    }
    if (sl_realized(el) && !(p && p->state == TW_PENDING)) {
        spec->after = p ? p->id : 0;
        slate_cmd_t c = { .kind = SLATE_CMD_TWEEN, .id = el->id, .data = spec };
        if (!sl_push(&c)) {
            zval_ptr_dtor(rv);
            ZVAL_UNDEF(rv);
            return false;
        }
        t->state = TW_RUNNING;
    } else {
        /* its element is not drawn yet (or the tween it follows waits): sent when both are there (sl_anim_realized) */
        t->state = TW_PENDING;
        t->spec = emalloc(sizeof *spec);
        memcpy(t->spec, spec, sizeof *spec);
    }
    for (int i = 0; i < spec->n; i++) {
        t->props |= 1u << spec->props[i].prop;
    }
    GC_ADDREF(&t->std);
    t->held = true;
    GC_ADDREF(&el->std);
    t->el = &el->std;
    el->tweens++;
    if (p) {   /* appended to the end of the chain, as the engine does */
        p->next = t;
        t->prev = p;
    }
    return true;
}

bool sl_tween_instant(zval *rv, sl_obj_t *el)
{
    (void)el;
    if (!ready()) {
        return false;
    }
    sl_anim_flush();
    object_init_ex(rv, sl_tween_ce);
    sl_tween_t *t = tween_of(Z_OBJ_P(rv));
    if (++s_next_id == 0) {
        s_next_id = 1;
    }
    t->id = s_next_id;
    zend_hash_index_update_ptr(&s_all, t->id, t);
    t->listed = true;
    /* as a pending tween whose properties were all written: done now, its TweenDone queued */
    GC_ADDREF(&t->std);
    t->held = true;
    t->state = TW_RUNNING;
    finish(t);
    queue_done(t->id);
    if (!listed(t->id)) {
        unhold(t);   /* no memory for its entry: no TweenDone */
    }
    return true;
}

bool sl_tween_running(uint32_t id)
{
    sl_tween_t *t = find(id);
    return t && t->state == TW_RUNNING && !listed(id);
}

void sl_tween_silenced(uint32_t id)
{
    sl_tween_t *t = find(id);
    if (t && t->state == TW_RUNNING) {
        stop_chain(t);   /* may free t */
    }
}

/* Sends a pending tween whose element is realized and whose predecessor is not pending: the values before
   animate() first (the element was realized with the final ones), then the TWEEN; then the pending tweens chained
   after it whose elements are realized. false with an exception. */
static bool send_pending(sl_tween_t *t)
{
    while (t && t->state == TW_PENDING && sl_realized(sl_obj(t->el))) {
        sl_obj_t *el = sl_obj(t->el);
        slate_tween_spec_t *sp = t->spec;
        /* the `from` of a property is set only when no earlier tween of the element that is still alive moves it:
           a later one starts from what the earlier leaves (decided now: an earlier one may have been stopped) */
        uint32_t taken = 0;
        sl_tween_t *x;
        ZEND_HASH_FOREACH_PTR(&s_all, x) {
            if (x != t && x->id < t->id && ALIVE(x) && x->el == t->el) {
                taken |= x->props;
            }
        } ZEND_HASH_FOREACH_END();
        for (int i = 0; i < sp->n; i++) {
            slate_tween_prop_t *tp = &sp->props[i];
            if (taken & (1u << tp->prop)) {
                continue;
            }
            slate_cmd_t c = { .kind = SLATE_CMD_SET, .id = el->id, .prop = tp->prop };
            if (tp->type == SLATE_VAL_COLOR) {
                c.v = tp->prop == SLATE_PROP_BORDER_COLOR || el->base == SL_LINE || el->base == SL_TEXT
                          ? slate_v_color(tp->from.argb)
                          : slate_v_paint((slate_paint_t){ tp->from.argb, NULL });
            } else {
                c.v = slate_v_num(tp->from.num);
            }
            if (!sl_push(&c)) {
                return false;
            }
        }
        sl_tween_t *nx = t->next;
        if (sp->n == 0) {
            /* every property was written meanwhile: nothing to move, it ends now */
            t->state = TW_RUNNING;
            finish(t);
            queue_done(t->id);
            if (!listed(t->id)) {
                unhold(t);
            }
            t = nx;
            continue;
        }
        sp->after = t->prev && t->prev->state == TW_RUNNING ? t->prev->id : 0;
        slate_cmd_t c = { .kind = SLATE_CMD_TWEEN, .id = el->id, .data = sp };
        if (!sl_push(&c)) {
            return false;
        }
        t->state = TW_RUNNING;
        efree(t->spec);
        t->spec = NULL;
        t = nx;
    }
    return true;
}

bool sl_anim_realized(sl_obj_t *o)
{
    if (!s_ready || !o->tweens) {
        return true;
    }
    uint32_t n = 0, cap = 0, *ids = NULL;
    sl_tween_t *t;
    ZEND_HASH_FOREACH_PTR(&s_all, t) {   /* in the order they were made */
        if (t->state == TW_PENDING && t->el == &o->std && !(t->prev && t->prev->state == TW_PENDING)) {
            if (n == cap) {
                cap = cap ? cap * 2 : 4;
                ids = erealloc(ids, cap * sizeof *ids);
            }
            ids[n++] = t->id;
        }
    } ZEND_HASH_FOREACH_END();
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) {
        sl_tween_t *x = find(ids[i]);
        ok = !x || send_pending(x);
    }
    if (ids) {
        efree(ids);
    }
    return ok;
}

void sl_anim_written(sl_obj_t *o, slate_prop_t prop)
{
    sl_tween_t *t;
    ZEND_HASH_FOREACH_PTR(&s_all, t) {
        if (t->state == TW_PENDING && t->el == &o->std) {
            slate_tween_spec_t *sp = t->spec;
            for (int i = 0; i < sp->n; i++) {
                if (sp->props[i].prop == prop) {
                    t->props &= ~(1u << prop);
                    memmove(&sp->props[i], &sp->props[i + 1], (size_t)(sp->n - i - 1) * sizeof sp->props[0]);
                    sp->n--;
                    break;
                }
            }
        }
    } ZEND_HASH_FOREACH_END();
}

/* ---- Tween ---- */

static bool timeout_ok(zend_long ms)
{
    if (ms < 0) {
        zend_argument_value_error(1, "must be greater than or equal to 0");
        return false;
    }
    return true;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_private_ctor, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(SlateTween, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

/* A pending tween stopped where it is (not toEnd): it never moved its element, so the next pending tween of the
   element on each of its properties starts from where this one would have (its `from`). */
static void pending_handover(sl_tween_t *t)
{
    for (int i = 0; i < t->spec->n; i++) {
        const slate_tween_prop_t *tp = &t->spec->props[i];
        sl_tween_t *best = NULL, *x;
        ZEND_HASH_FOREACH_PTR(&s_all, x) {
            if (x->id > t->id && x->state == TW_PENDING && x->el == t->el && (x->props & (1u << tp->prop))
                && (!best || x->id < best->id)) {
                best = x;
            }
        } ZEND_HASH_FOREACH_END();
        if (best) {
            for (int j = 0; j < best->spec->n; j++) {
                if (best->spec->props[j].prop == tp->prop) {
                    best->spec->props[j].from = tp->from;
                }
            }
        }
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tween_stop, 0, 0, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, toEnd, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

/* stop(): the tween and the ones chained after it, without a TweenDone; toEnd: at their final values. */
PHP_METHOD(SlateTween, stop)
{
    bool to_end = false;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(to_end)
    ZEND_PARSE_PARAMETERS_END();
    sl_anim_flush();
    sl_tween_t *t = tween_of(Z_OBJ_P(ZEND_THIS));
    if (!ALIVE(t)) {
        return;
    }
    /* a pending one is not in the engine (its chain neither: it waits for it); its values are the final ones */
    bool ok = true;
    if (t->state == TW_RUNNING) {
        slate_cmd_t c = { .kind = SLATE_CMD_TWEEN_STOP, .id = t->id, .v = slate_v_bool(to_end) };
        ok = sl_push(&c);
    }
    if (!to_end) {
        for (sl_tween_t *x = t; x; x = x->next) {
            if (x->state == TW_PENDING) {
                pending_handover(x);
            }
        }
    }
    stop_chain(t);
    if (!ok) {
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tween_wait, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "5000")
ZEND_END_ARG_INFO()

/* wait(): until the tween ends (true), at most $timeout_ms; false at once when it was stopped or never ends. The
   pending changes are handed over first; no event is delivered meanwhile. */
PHP_METHOD(SlateTween, wait)
{
    zend_long ms = 5000;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!timeout_ok(ms)) {
        RETURN_THROWS();
    }
    sl_anim_flush();
    sl_tween_t *t = tween_of(Z_OBJ_P(ZEND_THIS));
    if (ALIVE(t) && !t->endless) {
        if (sl_g.engine && !sl_check()) {
            RETURN_THROWS();
        }
        sl_deliver();   /* its TWEEN (or a remove() that stops it) may still be pending */
#if SIZEOF_ZEND_LONG > 4
        if (ms > INT64_C(1000000000000)) {
            ms = (zend_long)INT64_C(1000000000000);
        }
#endif
        int64_t deadline = bm_hal_now_us() + (int64_t)ms * 1000;
        for (;;) {
            pull(__atomic_load_n(&s_engine, __ATOMIC_ACQUIRE));
            if (t->state == TW_RUNNING && listed(t->id)) {
                finish(t);   /* the entry stays: its TweenDone still comes */
            }
            if (!ALIVE(t)) {
                break;
            }
            int64_t left = deadline - bm_hal_now_us();
            if (left <= 0) {
                break;
            }
            uint32_t wait_ms = (uint32_t)((left + 999) / 1000);
            uint8_t tok;
            bm_hal_queue_receive(s_wake, &tok, wait_ms < 20 ? wait_ms : 20);
        }
        sl_anim_flush();
    }
    RETURN_BOOL(t->state == TW_DONE);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_tween_then, 0, 2, Baremetal\\Slate\\Tween, 0)
    ZEND_ARG_OBJ_INFO(0, e, Baremetal\\Slate\\Element, 0)
    ZEND_ARG_TYPE_INFO(0, props, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ms, IS_LONG, 0, "300")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, ease, Baremetal\\Slate\\Ease, 0, "\\Baremetal\\Slate\\Ease::OutCubic")
ZEND_END_ARG_INFO()

/* then(): a tween of $e that starts when this one (and what is already chained after it) ends; at once when this
   one has ended or stopped. */
PHP_METHOD(SlateTween, then)
{
    zend_object *eo, *ease = NULL;
    HashTable *props;
    zend_long ms = 300;
    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_OBJ_OF_CLASS(eo, sl_element_ce)
        Z_PARAM_ARRAY_HT(props)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
        Z_PARAM_OBJ_OF_CLASS(ease, sl_ease_ce)
    ZEND_PARSE_PARAMETERS_END();
    if (!sl_anim_times(ms, 0, 0)) {
        RETURN_THROWS();
    }
    slate_tween_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.ms = (int32_t)ms;
    spec.ease = sl_ease_of(ease);
    if (!sl_anim_collect(sl_obj(eo), props, false, &spec)
        || !sl_tween_make(return_value, sl_obj(eo), &spec, Z_OBJ_P(ZEND_THIS))) {
        RETURN_THROWS();
    }
}

static const zend_function_entry tween_methods[] = {
    PHP_ME(SlateTween, __construct, arginfo_private_ctor, ZEND_ACC_PRIVATE)
    PHP_ME(SlateTween, stop, arginfo_tween_stop, ZEND_ACC_PUBLIC)
    PHP_ME(SlateTween, wait, arginfo_tween_wait, ZEND_ACC_PUBLIC)
    PHP_ME(SlateTween, then, arginfo_tween_then, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static zend_object *tween_create(zend_class_entry *ce)
{
    sl_tween_t *t = zend_object_alloc(sizeof(sl_tween_t), ce);
    memset(t, 0, XtOffsetOf(sl_tween_t, std));
    zend_object_std_init(&t->std, ce);
    object_properties_init(&t->std, ce);
    t->std.handlers = &tween_handlers;
    t->state = TW_STOPPED;
    ZVAL_FALSE(OBJ_PROP(&t->std, s_off_done));
    return &t->std;
}

static void tween_free(zend_object *obj)
{
    sl_tween_t *t = tween_of(obj);
    if (s_ready && t->listed) {
        zend_hash_index_del(&s_all, t->id);
    }
    t->listed = false;
    if (t->spec) {
        efree(t->spec);
        t->spec = NULL;
    }
    if (t->next) {
        t->next->prev = NULL;
    }
    if (t->prev) {
        t->prev->next = NULL;
    }
    if (t->el) {   /* only at the end of the request: a running Tween is held */
        zend_object *el = t->el;
        t->el = NULL;
        OBJ_RELEASE(el);
    }
    zend_object_std_dtor(obj);
}

static HashTable *tween_gc(zend_object *obj, zval **table, int *n)
{
    sl_tween_t *t = tween_of(obj);
    zend_get_gc_buffer *buf = zend_get_gc_buffer_create();
    if (t->el) {
        zend_get_gc_buffer_add_obj(buf, t->el);
    }
    zend_get_gc_buffer_use(buf, table, n);
    return obj->properties;   /* $done: a bool */
}

/* ---- transitions ----

   Slate::show($s, $t) pushes a SHOW with the transition and records it here (from, to: the scenes, referenced). The
   record runs until the engine reports its end (slate_engine_transition_done, drained by the listener above into a
   list of ends): an end with a scene left gets a TransitionDone event (8 bytes: the two roots' ids), whose filler
   marks the record ended and fills $from and $to; Slate's watch of TransitionDone, which runs before the PHP
   listeners, then calls onLeave() of the scene left (unless a show() during the transition already did) and drops
   the record. An end from no scene (the first show()) has no event: the idle hook marks it ended. As for the tweens,
   an end whose event was never delivered (a full pool, init-loop) is marked ended by the idle hook; its onLeave()
   runs at the next show() or commit() (sl_trans_settle), its TransitionDone is lost.

   While a record runs (from show() on: the engine applies the SHOW later) the touch is ignored (sl_trans_running,
   which applies the ends already reported first: the idle hook may not have run since). show() reserves a node of
   the list of ends per record (sl_trans_reserve), so the engine's task never allocates and no end is lost; the
   engine reports an end for every SHOW with a transition (a cut too).
   The engine ends transitions in the order of their SHOWs: an end also ends the records of the same Slate before it
   (one that the engine could not start has no end of its own). */

zend_class_entry *sl_transition_ce, *sl_direction_ce, *sl_transition_done_ce;

static zend_object_handlers transition_handlers;

typedef struct {
    slate_trans_t tr;
    zend_object std;
} sl_transition_t;

static sl_transition_t *transition_of(zend_object *o)
{
    return (sl_transition_t *)((char *)o - XtOffsetOf(sl_transition_t, std));
}

/* shared (s_lock): the ends the engine reported and that the PHP task has not taken */
typedef struct sl_trend {
    uint32_t from, to;
    bool evented;                 /* a TransitionDone was emitted for it */
    struct sl_trend *next;
} sl_trend_t;

static sl_trend_t *s_tr_head, *s_tr_tail;
static sl_trend_t *s_tr_spare;    /* free nodes: show() reserves one per record before its SHOW, so the engine's
                                     task never allocates and no end is lost (created as needed, never freed) */
static uint32_t s_tr_spare_n;     /* nodes in s_tr_spare */
static int s_tr_emitting;         /* ends between their append and the end of their emit */
static bool s_tr_accepting;       /* a request runs with the event set up */
static uint16_t s_tr_tag;

/* the PHP task's records, in the order of their show() */
typedef struct {
    uint32_t from_id, to_id;
    zend_object *from, *to;       /* references; from NULL: no scene was shown */
    uint32_t gen;                 /* the Slate (sl_g.gen) whose engine runs it */
    bool ended;                   /* the engine reported its end (or one after it, of the same Slate) */
    bool left;                    /* onLeave() of from already ran (a show() during it) */
} sl_trec_t;

static bool s_tr_ready;
static sl_trec_t *s_trec;
static uint32_t s_trec_n, s_trec_cap;

/* An end of the engine, into the list (with its TransitionDone when a scene was left). Any task (not an ISR). */
static void queue_tr(uint32_t from, uint32_t to)
{
    bm_hal_lock_t *l = lock();
    sl_trend_t *n = s_tr_spare;
    if (!s_tr_accepting || !n) {
        bm_hal_lock_exit(l);   /* no spare: never, a node is reserved per record (sl_trans_reserve) */
        return;
    }
    s_tr_spare = n->next;
    s_tr_spare_n--;
    n->from = from;
    n->to = to;
    n->evented = from != 0;
    n->next = NULL;
    if (s_tr_tail) {
        s_tr_tail->next = n;
    } else {
        s_tr_head = n;
    }
    s_tr_tail = n;
    bool emit = n->evented;
    if (emit) {
        s_tr_emitting++;
    }
    uint16_t tag = s_tr_tag;
    bm_hal_lock_exit(l);
    if (emit) {
        uint32_t d[2] = { from, to };
        bm_events_emit_data(tag, NULL, d, sizeof d);   /* dropped: the idle hook ends the record */
        l = lock();
        s_tr_emitting--;
        bm_hal_lock_exit(l);
    }
}

/* Every end the engine has queued, into the list. Any task: the engine's listener, the PHP task. */
static void pull_tr(slate_engine_t *e)
{
    if (!e || !__atomic_load_n(&s_lock, __ATOMIC_ACQUIRE)) {
        return;
    }
    uint32_t from, to;
    while (slate_engine_transition_done(e, &from, &to)) {
        queue_tr(from, to);
    }
}

/* The record the end (from, to) is for: the first running one with those ids; the running ones before it end too.
   Its index, or -1. PHP task (plain C). */
static int trec_end(uint32_t from, uint32_t to)
{
    for (uint32_t i = 0; i < s_trec_n; i++) {
        if (!s_trec[i].ended && s_trec[i].from_id == from && s_trec[i].to_id == to) {
            /* only records of the same Slate: an engine orders its own ends; a record kept from a Slate that stopped
               (its end reported, its event not delivered yet) ends with that event */
            for (uint32_t j = 0; j <= i; j++) {
                if (s_trec[j].gen == s_trec[i].gen) {
                    s_trec[j].ended = true;
                }
            }
            return (int)i;
        }
    }
    return -1;
}

/* Takes the first end (from, to) out of the list. */
static void take_tr(uint32_t from, uint32_t to)
{
    bm_hal_lock_t *l = lock();
    sl_trend_t *prev = NULL, *n = s_tr_head;
    while (n && !(n->from == from && n->to == to && n->evented)) {
        prev = n;
        n = n->next;
    }
    if (n) {
        if (prev) {
            prev->next = n->next;
        } else {
            s_tr_head = n->next;
        }
        if (s_tr_tail == n) {
            s_tr_tail = prev;
        }
        n->next = s_tr_spare;
        s_tr_spare = n;
        s_tr_spare_n++;
    }
    bm_hal_lock_exit(l);
}

/* The idle hook's part (plain C): the ends without an event, and those whose events are gone undelivered, end their
   records (their onLeave() waits for sl_trans_settle). */
void sl_trans_idle(void)
{
    if (!s_tr_ready) {
        return;
    }
    evt_tag_stats_t st;
    evt_tag_stats(s_tr_tag, &st);
    bm_hal_lock_t *l = lock();
    /* an evented end is taken only when every emit is over and none is queued: its event was given back or dropped */
    bool dropped = s_tr_emitting == 0 && st.queued == 0;
    sl_trend_t *take = NULL, **tail = &take, *prev = NULL, *n = s_tr_head;
    while (n) {
        sl_trend_t *next = n->next;
        if (!n->evented || dropped) {
            if (prev) {
                prev->next = next;
            } else {
                s_tr_head = next;
            }
            if (s_tr_tail == n) {
                s_tr_tail = prev;
            }
            n->next = NULL;
            *tail = n;
            tail = &n->next;
        } else {
            prev = n;
        }
        n = next;
    }
    bm_hal_lock_exit(l);
    uint32_t k = 0;
    for (sl_trend_t *x = take; x; x = x->next) {
        trec_end(x->from, x->to);
        k++;
    }
    if (take) {
        l = lock();
        *tail = s_tr_spare;
        s_tr_spare = take;
        s_tr_spare_n += k;
        bm_hal_lock_exit(l);
    }
}

/* The filler of TransitionDone (PHP task, before the delivery): the record ends, $from and $to. */
static void fill_tr(zend_object *ev, zend_object *dev, const evt_t *slot)
{
    (void)dev;
    size_t len;
    const void *d = bm_events_slot_data(slot, &len);
    uint32_t ids[2] = { 0, 0 };
    if (d && len == sizeof ids) {
        memcpy(ids, d, sizeof ids);
    }
    int i = -1;
    if (s_tr_ready) {
        take_tr(ids[0], ids[1]);
        i = trec_end(ids[0], ids[1]);
        for (uint32_t j = 0; i < 0 && j < s_trec_n; j++) {   /* ended already (by a later end): still its own */
            if (s_trec[j].ended && s_trec[j].from_id == ids[0] && s_trec[j].to_id == ids[1] && s_trec[j].from) {
                i = (int)j;
            }
        }
    }
    zval zf, zt;
    if (i >= 0 && s_trec[i].from) {
        ZVAL_OBJ(&zf, s_trec[i].from);
        ZVAL_OBJ(&zt, s_trec[i].to);
        bm_events_set_zval(ev, sl_transition_done_ce, "from", &zf);   /* the event's own references */
        bm_events_set_zval(ev, sl_transition_done_ce, "to", &zt);
    } else {
        /* no record: only when the request's records are gone (RSHUTDOWN, or a record dropped by a Slate stop whose
           end was reported anyway); stand-ins (new empty Scenes), so the event is whole */
        object_init_ex(&zf, sl_scene_ce);
        object_init_ex(&zt, sl_scene_ce);
        bm_events_set_zval(ev, sl_transition_done_ce, "from", &zf);
        bm_events_set_zval(ev, sl_transition_done_ce, "to", &zt);
        zval_ptr_dtor(&zf);
        zval_ptr_dtor(&zt);
    }
}

static void call_leave(zend_object *scene)
{
    zend_function *fn = zend_hash_str_find_ptr(&scene->ce->function_table, "onleave", sizeof("onleave") - 1);
    if (fn) {
        zend_call_known_instance_method_with_0_params(fn, scene, NULL);
    }
}

static void trec_release(sl_trec_t *r)
{
    if (r->from) {
        OBJ_RELEASE(r->from);
    }
    if (r->to) {
        OBJ_RELEASE(r->to);
    }
}

bool sl_trans_settle(void)
{
    while (s_tr_ready && !EG(exception)) {
        uint32_t i = 0;
        while (i < s_trec_n && !s_trec[i].ended) {
            i++;
        }
        if (i == s_trec_n) {
            break;
        }
        sl_trec_t r = s_trec[i];   /* out first: onLeave() may show() */
        memmove(&s_trec[i], &s_trec[i + 1], (s_trec_n - i - 1) * sizeof *s_trec);
        s_trec_n--;
        if (r.from && !r.left) {
            call_leave(r.from);
        }
        trec_release(&r);
    }
    return !EG(exception);
}

/* Slate's watch of TransitionDone (before the PHP listeners): onLeave() of the scene the transition left. */
static void watch_tr(zend_object *event, void *arg)
{
    (void)event;
    (void)arg;
    sl_trans_settle();
}

bool sl_trans_leave(void)
{
    for (uint32_t i = 0; i < s_trec_n && !EG(exception); i++) {
        if (!s_trec[i].ended && !s_trec[i].left && s_trec[i].from) {
            s_trec[i].left = true;   /* before the call: never twice */
            zend_object *from = s_trec[i].from;
            GC_ADDREF(from);   /* onLeave() may show(): the array moves */
            call_leave(from);
            OBJ_RELEASE(from);
        }
    }
    return !EG(exception);
}

static bool any_running(void)
{
    for (uint32_t i = 0; i < s_trec_n; i++) {
        if (!s_trec[i].ended) {
            return true;
        }
    }
    return false;
}

bool sl_trans_running(void)
{
    /* the ends already reported first: the reactor may have blocked (after its idle hook) before the engine ended
       one without an event, and woken only for this touch */
    if (!any_running()) {
        return false;
    }
    sl_trans_idle();
    return any_running();
}

bool sl_trans_ready(void)
{
    if (s_tr_ready) {
        return true;
    }
    if (!lock_ready()) {
        return false;
    }
    uint16_t tag;
    if (!bm_events_source_tag(sl_transition_done_ce->name, &tag)) {
        if (!EG(exception)) {
            zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Slate: cannot raise %s: too many event sources",
                                    ZSTR_VAL(sl_transition_done_ce->name));
        }
        return false;
    }
    bm_events_set_filler(tag, fill_tr);
    if (!bm_events_watch(sl_transition_done_ce, watch_tr, NULL)) {
        bm_events_set_filler(tag, NULL);
        zend_throw_exception(spl_ce_RuntimeException,
                             "Slate: cannot run a transition: the table of ext-events' watches is full", 0);
        return false;
    }
    if (!sl_idle_use(SL_IDLE_TRANS, true)) {
        /* ext-events keeps 4 hooks; Slate takes 1 for all its users */
        bm_events_unwatch(sl_transition_done_ce, watch_tr, NULL);
        bm_events_set_filler(tag, NULL);
        zend_throw_exception(spl_ce_RuntimeException,
                             "Slate: cannot run a transition: the table of ext-events' idle hooks is full", 0);
        return false;
    }
    bm_hal_lock_t *l = lock();
    s_tr_tag = tag;
    s_tr_accepting = true;
    bm_hal_lock_exit(l);
    s_tr_ready = true;
    return true;
}

bool sl_trans_reserve(void)
{
    /* a free node for each record whose end may still come (those not ended; one whose end is listed already holds
       its node: counted twice, harmless) and for the one about to be added; nodes still listed for records that are
       gone (an event not delivered yet) are not counted as free */
    uint32_t want = 1;
    for (uint32_t i = 0; i < s_trec_n; i++) {
        want += !s_trec[i].ended;
    }
    for (;;) {
        bm_hal_lock_t *l = lock();
        bool enough = s_tr_spare_n >= want;
        bm_hal_lock_exit(l);
        if (enough) {
            return true;
        }
        sl_trend_t *n = bm_hal_alloc(sizeof *n, BM_HAL_MEM_EXTERNAL);
        if (!n) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: out of memory for the transition", 0);
            return false;
        }
        l = lock();
        n->next = s_tr_spare;
        s_tr_spare = n;
        s_tr_spare_n++;
        bm_hal_lock_exit(l);
    }
}

void sl_trans_add(zend_object *from, zend_object *to)
{
    if (s_trec_n == s_trec_cap) {
        s_trec_cap = s_trec_cap ? s_trec_cap * 2 : 4;
        s_trec = erealloc(s_trec, s_trec_cap * sizeof *s_trec);
    }
    sl_trec_t *r = &s_trec[s_trec_n++];
    r->from_id = from ? sl_obj(from)->id : 0;
    r->to_id = sl_obj(to)->id;
    r->from = from;
    r->to = to;
    r->gen = sl_g.gen;
    r->ended = false;
    r->left = false;
    if (from) {
        GC_ADDREF(from);
    }
    GC_ADDREF(to);
}

/* The engine stopped (sl_anim_engine(NULL)): a record whose end the engine did not report never ends (no onLeave(),
   no event): it goes; the others stay for their event or the idle hook. Plain C callers: the references go to the
   trash. */
static void trans_stopped(void)
{
    if (!s_tr_ready) {
        return;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < s_trec_n; i++) {
        sl_trec_t *r = &s_trec[i];
        bool reported = r->ended;
        if (!reported) {
            bm_hal_lock_t *l = lock();
            for (sl_trend_t *n = s_tr_head; n && !reported; n = n->next) {
                reported = n->from == r->from_id && n->to == r->to_id;
            }
            bm_hal_lock_exit(l);
        }
        if (reported) {
            s_trec[k++] = *r;
        } else {
            if (r->from) {
                trash(r->from);
            }
            trash(r->to);
        }
    }
    s_trec_n = k;
}

static void trans_rshutdown(void)
{
    if (!s_tr_ready) {
        return;
    }
    sl_idle_use(SL_IDLE_TRANS, false);
    bm_events_unwatch(sl_transition_done_ce, watch_tr, NULL);
    bm_hal_lock_t *l = lock();
    s_tr_accepting = false;
    if (s_tr_tail) {   /* the nodes stay for the next request */
        for (sl_trend_t *x = s_tr_head; x; x = x->next) {
            s_tr_spare_n++;
        }
        s_tr_tail->next = s_tr_spare;
        s_tr_spare = s_tr_head;
    }
    s_tr_head = s_tr_tail = NULL;
    bm_hal_lock_exit(l);
    bm_events_set_filler(s_tr_tag, NULL);
    s_tr_ready = false;
    for (uint32_t i = 0; i < s_trec_n; i++) {
        if (s_trec[i].from) {
            trash(s_trec[i].from);
        }
        trash(s_trec[i].to);
    }
    s_trec_n = 0;
    if (s_trec) {
        efree(s_trec);
        s_trec = NULL;
        s_trec_cap = 0;
    }
}

/* ---- Transition, Direction, TransitionDone ---- */

static const char *const dir_names[SLATE_DIR_COUNT] = { "Left", "Right", "Up", "Down" };

static uint8_t dir_of(zend_object *d)
{
    zval *name = zend_enum_fetch_case_name(d);
    for (int i = 0; i < SLATE_DIR_COUNT; i++) {
        if (Z_TYPE_P(name) == IS_STRING && strcmp(Z_STRVAL_P(name), dir_names[i]) == 0) {
            return (uint8_t)i;
        }
    }
    return SLATE_DIR_LEFT;
}

bool sl_transition_of(zend_object *t, slate_trans_t *out)
{
    if (!t) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = transition_of(t)->tr;
    return out->kind != SLATE_TRANS_NONE;
}

/* A Transition into return_value: ms checked (\ValueError of the Global rules). false with an exception. */
static bool transition_make(zval *rv, uint8_t kind, zend_object *dir, zend_long ms, zend_object *ease)
{
    if (!sl_anim_times(ms, 0, 0)) {
        return false;
    }
    object_init_ex(rv, sl_transition_ce);
    sl_transition_t *t = transition_of(Z_OBJ_P(rv));
    t->tr.kind = kind;
    t->tr.dir = dir ? dir_of(dir) : SLATE_DIR_LEFT;
    t->tr.ease = sl_ease_of(ease);
    t->tr.ms = (int32_t)ms;
    return true;
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_transition_fade, 0, 0, Baremetal\\Slate\\Transition, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ms, IS_LONG, 0, "300")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, ease, Baremetal\\Slate\\Ease, 0, "\\Baremetal\\Slate\\Ease::OutCubic")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_transition_slide, 0, 1, Baremetal\\Slate\\Transition, 0)
    ZEND_ARG_OBJ_INFO(0, d, Baremetal\\Slate\\Direction, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ms, IS_LONG, 0, "300")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, ease, Baremetal\\Slate\\Ease, 0, "\\Baremetal\\Slate\\Ease::OutCubic")
ZEND_END_ARG_INFO()

PHP_METHOD(SlateTransition, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

/* fade(): the new scene over the old one, its opacity from 0 to 1. */
PHP_METHOD(SlateTransition, fade)
{
    zend_long ms = 300;
    zend_object *ease = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
        Z_PARAM_OBJ_OF_CLASS(ease, sl_ease_ce)
    ZEND_PARSE_PARAMETERS_END();
    if (!transition_make(return_value, SLATE_TRANS_FADE, NULL, ms, ease)) {
        RETURN_THROWS();
    }
}

static void transition_slide(INTERNAL_FUNCTION_PARAMETERS, uint8_t kind)
{
    zend_object *dir, *ease = NULL;
    zend_long ms = 300;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_OBJ_OF_CLASS(dir, sl_direction_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
        Z_PARAM_OBJ_OF_CLASS(ease, sl_ease_ce)
    ZEND_PARSE_PARAMETERS_END();
    if (!transition_make(return_value, kind, dir, ms, ease)) {
        RETURN_THROWS();
    }
}

/* push(): the new scene comes in from the side opposite to $d and pushes the old one out towards $d. */
PHP_METHOD(SlateTransition, push)
{
    transition_slide(INTERNAL_FUNCTION_PARAM_PASSTHRU, SLATE_TRANS_PUSH);
}

/* cover(): the new scene slides in towards $d over the old one, which stays. */
PHP_METHOD(SlateTransition, cover)
{
    transition_slide(INTERNAL_FUNCTION_PARAM_PASSTHRU, SLATE_TRANS_COVER);
}

/* uncover(): the old scene slides away towards $d and uncovers the new one, which stays. */
PHP_METHOD(SlateTransition, uncover)
{
    transition_slide(INTERNAL_FUNCTION_PARAM_PASSTHRU, SLATE_TRANS_UNCOVER);
}

static const zend_function_entry transition_methods[] = {
    PHP_ME(SlateTransition, __construct, arginfo_private_ctor, ZEND_ACC_PRIVATE)
    PHP_ME(SlateTransition, fade, arginfo_transition_fade, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(SlateTransition, push, arginfo_transition_slide, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(SlateTransition, cover, arginfo_transition_slide, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(SlateTransition, uncover, arginfo_transition_slide, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static zend_object *transition_create(zend_class_entry *ce)
{
    sl_transition_t *t = zend_object_alloc(sizeof(sl_transition_t), ce);
    memset(t, 0, XtOffsetOf(sl_transition_t, std));
    zend_object_std_init(&t->std, ce);
    object_properties_init(&t->std, ce);
    t->std.handlers = &transition_handlers;
    return &t->std;
}

static void transitions_register(void)
{
    sl_direction_ce = zend_register_internal_enum("Baremetal\\Slate\\Direction", IS_UNDEF, NULL);
    for (int i = 0; i < SLATE_DIR_COUNT; i++) {
        zend_enum_add_case_cstr(sl_direction_ce, dir_names[i], NULL);
    }

    memcpy(&transition_handlers, zend_get_std_object_handlers(), sizeof transition_handlers);
    transition_handlers.offset = XtOffsetOf(sl_transition_t, std);
    transition_handlers.clone_obj = NULL;
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Transition", transition_methods);
    sl_transition_ce = zend_register_internal_class(&ce);
    sl_transition_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    sl_transition_ce->create_object = transition_create;

    if (!bm_events_base_ce()) {
        return;   /* ext-events is a required module: never the case */
    }
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "TransitionDone", NULL);
    sl_transition_done_ce = zend_register_internal_class_ex(&ce, bm_events_base_ce());
    sl_transition_done_ce->ce_flags |= ZEND_ACC_FINAL;
    static const char *const props[2] = { "from", "to" };
    for (int i = 0; i < 2; i++) {
        zval undef;
        ZVAL_UNDEF(&undef);
        zend_declare_typed_property(sl_transition_done_ce, zend_string_init_interned(props[i], strlen(props[i]), 1),
                                    &undef, ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                    (zend_type)ZEND_TYPE_INIT_CLASS(sl_scene_ce->name, 0, 0));
    }
}

/* ---- registration ---- */

void sl_anim_register(void)
{
    slate_engine_set_tween_listener(on_tween_done, NULL);   /* copied by every slate_engine_start */

    sl_ease_ce = zend_register_internal_enum("Baremetal\\Slate\\Ease", IS_UNDEF, NULL);
    for (int i = 0; i < SLATE_EASE_COUNT; i++) {
        zend_enum_add_case_cstr(sl_ease_ce, ease_names[i], NULL);
    }

    memcpy(&tween_handlers, zend_get_std_object_handlers(), sizeof tween_handlers);
    tween_handlers.offset = XtOffsetOf(sl_tween_t, std);
    tween_handlers.free_obj = tween_free;
    tween_handlers.clone_obj = NULL;
    tween_handlers.get_gc = tween_gc;
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Tween", tween_methods);
    sl_tween_ce = zend_register_internal_class(&ce);
    sl_tween_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    sl_tween_ce->create_object = tween_create;
    zval undef;
    ZVAL_UNDEF(&undef);
    zend_property_info *info = zend_declare_typed_property(
        sl_tween_ce, zend_string_init_interned("done", sizeof("done") - 1, 1), &undef,
        ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL, (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0));
    s_off_done = info->offset;

    if (!bm_events_base_ce()) {
        return;   /* ext-events is a required module: never the case */
    }
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "TweenDone", NULL);
    sl_tween_done_ce = zend_register_internal_class_ex(&ce, bm_events_base_ce());
    sl_tween_done_ce->ce_flags |= ZEND_ACC_FINAL;
    ZVAL_UNDEF(&undef);
    zend_declare_typed_property(sl_tween_done_ce, zend_string_init_interned("tween", sizeof("tween") - 1, 1), &undef,
                                ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                (zend_type)ZEND_TYPE_INIT_CLASS(sl_tween_ce->name, 0, 0));

    transitions_register();
}
