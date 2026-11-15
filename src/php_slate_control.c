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
 * Controls: the abstract class Control (a Group) and what every control shares. A control is one PHP object, an
 * engine GROUP of a base of its own (php_slate_elements.c: SL_CONTROL for a user class that extends Control directly,
 * SL_BUTTON... for Slate's controls and their subclasses). Its sl_ctl_t (sl_obj_t.ctl) holds:
 *   - its parts: engine elements without a PHP object (a background, a text, an icon...), made by the control's C
 *     code with ids of their own (sl_id_new()). They reach the engine when the control is realized: NEW of every part,
 *     then ops->realize (their SETs, from the control's state), then the ADD of each part to its parent (the control,
 *     or a GROUP part), all before the control's PHP children, which go on top. A write of the control while it is
 *     realized sends the SETs that changed at once (sl_part_set); before that only its state changes. Freeing the
 *     control sends FREE of every part (a FREE of the group would leave them alive under their ids);
 *   - the touch: deliver() of php_slate_touch.c calls sl_ctl_touch() before the method and the callback of each kind:
 *     Press calls pressed(true), Release and Cancel pressed(false) (the PHP method: a subclass's override wins; Slate's
 *     controls declare their own, which draws the pressed look through ops->pressed), then ops->touch;
 *   - the notifications: notify() calls onChange() (a method, empty here) and then the callback of changed(); Slate's
 *     controls call it for the user's gestures only;
 *   - $enabled (php_slate_elements.c sends OPACITY times SLATE_CTL_DISABLED and TOUCHABLE false; can_take() of
 *     php_slate_touch.c refuses a control that is not enabled, and an element inside one);
 *   - $theme (protected readonly): the running Slate's theme when the object is made.
 *
 * add() and remove() are Group's for a user control (and a Dialog): Slate's other controls build their own parts.
 *
 * Internal tweens. A control animates its parts with tweens of internal ids (sl_part_tween: SL_TWEEN_CTL, no
 * TweenDone). One whose end the control needs (a Dialog's close; the toast's end, php_slate.c) carries
 * SL_TWEEN_CTL_CB and has a waiter (sl_ctl_wait): the tween listener (php_slate_anim.c, pull(), the engine's task)
 * queues its id here (a fixed queue of SL_DUE_MAX under a HAL lock made once) and emits one HoldDue; Slate's watch of
 * HoldDue (php_slate_touch.c) runs sl_ctl_due_run() on the PHP task, which runs the waiter of each id. A full queue
 * keeps the ids it has and counts the new one lost: the next drain then also runs every waiter whose time is past.
 */
#include <string.h>

#include "php.h"
#include "zend_closures.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#include "bm_events_hal.h"

#include "controls.h"
#include "php_slate.h"

#if SLATE_HAS_CONTROLS

zend_class_entry *sl_control_ce;

static zend_class_entry *s_ce[SL_BASE_COUNT];        /* Slate's control classes, by base */
static const sl_ctl_ops_t *s_ops[SL_BASE_COUNT];
static uint32_t s_enabled_off, s_theme_off;          /* slots of Control::$enabled, Control::$theme */
static zend_string *s_x_name, *s_y_name;

/* ---- the control's record ---- */

void sl_ctl_init(sl_obj_t *o)
{
    sl_ctl_t *c = ecalloc(1, sizeof *c);   /* zeroed: changed and cb2 UNDEF */
    c->ops = s_ops[o->base];
    o->ctl = c;
    zend_object *th = sl_theme_current();
    if (th) {
        zval *slot = OBJ_PROP(&o->std, s_theme_off);
        ZVAL_OBJ_COPY(slot, th);
    }
}

bool sl_ctl_need_slate(sl_obj_t *o)
{
    if (!sl_g.engine) {
        zend_throw_error(NULL, "%s needs a running Slate: create the Slate before its controls",
                         ZSTR_VAL(o->std.ce->name));
        return false;
    }
    return true;
}

sl_theme_t *sl_ctl_theme(sl_obj_t *o)
{
    zval *slot = OBJ_PROP(&o->std, s_theme_off);
    return Z_TYPE_P(slot) == IS_OBJECT ? sl_theme_of(Z_OBJ_P(slot)) : NULL;
}

bool sl_ctl_enabled(const sl_obj_t *o)
{
    return Z_TYPE_P(OBJ_PROP(&((sl_obj_t *)o)->std, s_enabled_off)) != IS_FALSE;
}

float sl_ctl_opacity(const sl_obj_t *o)
{
    return sl_ctl_enabled(o) ? 1.0f : SLATE_CTL_DISABLED;
}

/* ---- parts ---- */

int sl_part_add(sl_obj_t *o, slate_el_kind_t kind, int parent)
{
    sl_ctl_t *c = o->ctl;
    if (!c || c->nparts >= 127) {
        zend_throw_error(NULL, "%s: too many parts", ZSTR_VAL(o->std.ce->name));
        return -1;
    }
    if (c->nparts == c->cap) {
        c->cap = c->cap ? (uint16_t)(c->cap * 2) : 4;
        c->parts = erealloc(c->parts, c->cap * sizeof *c->parts);
    }
    sl_part_t *p = &c->parts[c->nparts];
    p->id = sl_id_new();
    p->kind = (uint8_t)kind;
    p->parent = (int8_t)parent;
    return c->nparts++;
}

bool sl_part_set(sl_obj_t *o, int i, slate_prop_t prop, slate_value_t v)
{
    if (!sl_realized(o) || i < 0 || i >= o->ctl->nparts) {
        return true;
    }
    slate_cmd_t c = { .kind = SLATE_CMD_SET, .id = o->ctl->parts[i].id, .prop = prop, .v = v };
    return sl_push(&c);
}

bool sl_part_ease(sl_obj_t *o, int i, uint32_t mask, int ms, slate_ease_t ease)
{
    if (!sl_realized(o) || i < 0 || i >= o->ctl->nparts) {
        return true;
    }
    slate_ease_spec_t es = { .mask = mask, .ms = ms, .ease = (uint8_t)ease };
    slate_cmd_t c = { .kind = SLATE_CMD_EASE, .id = o->ctl->parts[i].id, .data = &es };
    return sl_push(&c);
}

bool sl_ctl_realize(sl_obj_t *o)
{
    sl_ctl_t *c = o->ctl;
    if (c->down) {
        /* a press the previous Slate never ended (stopped mid-press): no stuck pressed look in this one */
        c->down = false;
        if (c->ops && c->ops->pressed) {
            c->ops->pressed(o, false);
        }
    }
    for (int i = 0; i < c->nparts; i++) {
        slate_cmd_t nc = { .kind = SLATE_CMD_NEW, .id = c->parts[i].id, .el = (slate_el_kind_t)c->parts[i].kind };
        if (!sl_push(&nc)) {
            return false;
        }
    }
    if (c->ops && c->ops->realize && !c->ops->realize(o)) {
        return false;
    }
    for (int i = 0; i < c->nparts; i++) {
        int par = c->parts[i].parent;
        slate_cmd_t add = { .kind = SLATE_CMD_ADD, .id = c->parts[i].id,
                            .parent = par >= 0 && par < c->nparts ? c->parts[par].id : o->id };
        if (!sl_push(&add)) {
            return false;
        }
    }
    return true;
}

void sl_ctl_free(sl_obj_t *o)
{
    sl_ctl_t *c = o->ctl;
    if (sl_realized(o)) {
        /* no exception from here: a refusal only leaves the part in the engine until it stops */
        for (int i = c->nparts - 1; i >= 0; i--) {
            slate_cmd_t fc = { .kind = SLATE_CMD_FREE, .id = c->parts[i].id };
            slate_cmd_push(slate_engine_cmds(sl_g.engine), &fc);
        }
    }
    if (c->st) {
        if (c->ops && c->ops->free) {
            c->ops->free(o);
        } else {
            efree(c->st);
        }
        c->st = NULL;
    }
    zval_ptr_dtor(&c->changed);
    zval_ptr_dtor(&c->cb2);
    if (c->parts) {
        efree(c->parts);
    }
    efree(c);
    o->ctl = NULL;
}

uint32_t sl_ctl_parent_of(sl_obj_t *o, sl_obj_t *child)
{
    sl_ctl_t *c = o->ctl;
    uint32_t id = c && c->ops && c->ops->child_parent ? c->ops->child_parent(o, child) : 0;
    return id ? id : o->id;
}

void sl_ctl_gc(sl_obj_t *o, zend_get_gc_buffer *buf)
{
    sl_ctl_t *c = o->ctl;
    if (Z_TYPE(c->changed) == IS_OBJECT) {
        zend_get_gc_buffer_add_zval(buf, &c->changed);
    }
    if (Z_TYPE(c->cb2) == IS_OBJECT) {
        zend_get_gc_buffer_add_zval(buf, &c->cb2);
    }
    if (c->ops && c->ops->gc) {
        c->ops->gc(o, buf);
    }
}

/* ---- internal tweens and their ends ---- */

bool sl_part_tween(sl_obj_t *o, int part, const slate_tween_prop_t *props, int n, int ms, int delay,
                   slate_ease_t ease, int repeat, uint32_t after, bool cb, uint32_t *id)
{
    *id = 0;
    if (!sl_realized(o) || part >= o->ctl->nparts || n < 1 || n > SLATE_TWEEN_PROPS_MAX) {
        return true;
    }
    uint32_t tid = sl_tween_ctl_id(cb);
    if (!tid) {
        return false;
    }
    slate_tween_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.tween = tid;
    spec.after = after;
    spec.ms = ms;
    spec.delay_ms = delay;
    spec.repeat = repeat;
    spec.ease = (uint8_t)ease;
    spec.n = (uint8_t)n;
    memcpy(spec.props, props, (size_t)n * sizeof *props);
    slate_cmd_t c = { .kind = SLATE_CMD_TWEEN, .id = part < 0 ? o->id : o->ctl->parts[part].id, .data = &spec };
    if (!sl_push(&c)) {
        return false;
    }
    *id = tid;
    return true;
}

bool sl_part_prepare(sl_obj_t *o, int part)
{
    if (!sl_realized(o) || part >= o->ctl->nparts) {
        return true;
    }
    slate_cmd_t c = { .kind = SLATE_CMD_PREPARE, .id = part < 0 ? o->id : o->ctl->parts[part].id };
    return sl_push(&c);
}

bool sl_part_tween_stop(uint32_t id, bool to_end)
{
    if (!id || !sl_g.engine) {
        return true;
    }
    slate_cmd_t c = { .kind = SLATE_CMD_TWEEN_STOP, .id = id, .v = slate_v_bool(to_end) };
    return sl_push(&c);
}

#define SL_DUE_MAX 32

/* shared (s_due_lock): the ids the listener queued and the PHP task has not taken */
static bm_hal_lock_t *s_due_lock;   /* created once, never freed */
static uint32_t s_due_q[SL_DUE_MAX];
static int s_due_n;
static uint32_t s_due_lost;

/* the PHP task's: the waiters, id -> sl_waiter_t* (emalloc) */
typedef struct {
    zend_object *obj;      /* a reference (NULL: none) */
    sl_due_fn_t fn;
    int64_t due_us;        /* when the end is due (bm_hal_now_us), for a lost id */
} sl_waiter_t;
static HashTable s_waiters;
static bool s_waiters_ready;

void sl_ctl_due(uint32_t id)
{
    bm_hal_lock_t *l = __atomic_load_n(&s_due_lock, __ATOMIC_ACQUIRE);
    if (!l) {
        return;
    }
    bm_hal_lock_enter(l);
    if (s_due_n < SL_DUE_MAX) {
        s_due_q[s_due_n++] = id;
    } else {
        s_due_lost++;   /* the oldest stay; this one's waiter runs when its time is past (sl_ctl_due_run) */
    }
    bm_hal_lock_exit(l);
}

bool sl_ctl_due_ready(void)
{
    if (!s_due_lock) {
        bm_hal_lock_t *l = bm_hal_lock_create();
        if (!l) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: no memory for the animations", 0);
            return false;
        }
        __atomic_store_n(&s_due_lock, l, __ATOMIC_RELEASE);
    }
    if (!s_waiters_ready) {
        zend_hash_init(&s_waiters, 8, NULL, NULL, 0);
        s_waiters_ready = true;
    }
    return true;
}

bool sl_ctl_wait(uint32_t id, zend_object *obj, sl_due_fn_t fn, int ms)
{
    if (!sl_ctl_due_ready()) {
        return false;
    }
    sl_waiter_t *w = emalloc(sizeof *w);
    w->obj = obj;
    w->fn = fn;
    w->due_us = bm_hal_now_us() + (int64_t)ms * 1000;
    if (obj) {
        GC_ADDREF(obj);
    }
    sl_waiter_t *old = zend_hash_index_find_ptr(&s_waiters, id);
    zend_hash_index_update_ptr(&s_waiters, id, w);
    if (old) {   /* never: ids are not reused */
        if (old->obj) {
            OBJ_RELEASE(old->obj);
        }
        efree(old);
    }
    return true;
}

/* The waiter of id out of the table (NULL: none). */
static sl_waiter_t *waiter_take(uint32_t id)
{
    if (!s_waiters_ready || !id) {
        return NULL;
    }
    sl_waiter_t *w = zend_hash_index_find_ptr(&s_waiters, id);
    if (w) {
        zend_hash_index_del(&s_waiters, id);
    }
    return w;
}

static void waiter_free(sl_waiter_t *w)
{
    if (w->obj) {
        OBJ_RELEASE(w->obj);
    }
    efree(w);
}

void sl_ctl_unwait(uint32_t id)
{
    sl_waiter_t *w = waiter_take(id);
    if (w) {
        waiter_free(w);
    }
}

/* Runs the waiter of id (none: an end nobody waits for any more). false when it threw. */
static bool due_one(uint32_t id)
{
    sl_waiter_t *w = waiter_take(id);
    if (!w) {
        return true;
    }
    w->fn(w->obj, id, false);
    waiter_free(w);
    return !EG(exception);
}

void sl_ctl_due_run(void)
{
    bm_hal_lock_t *l = __atomic_load_n(&s_due_lock, __ATOMIC_ACQUIRE);
    if (!l || !s_waiters_ready) {
        return;
    }
    uint32_t ids[SL_DUE_MAX];
    bm_hal_lock_enter(l);
    int n = s_due_n;
    memcpy(ids, s_due_q, (size_t)n * sizeof *ids);
    s_due_n = 0;
    uint32_t lost = s_due_lost;
    s_due_lost = 0;
    bm_hal_lock_exit(l);
    for (int i = 0; i < n; i++) {
        if (!due_one(ids[i])) {
            /* an exception stops there, as a handler's does: the rest wait for the next HoldDue */
            bm_hal_lock_enter(l);
            int rest = n - i - 1;
            if (rest > 0 && s_due_n + rest <= SL_DUE_MAX) {
                memmove(s_due_q + rest, s_due_q, (size_t)s_due_n * sizeof *s_due_q);
                memcpy(s_due_q, ids + i + 1, (size_t)rest * sizeof *ids);
                s_due_n += rest;
            } else if (rest > 0) {
                s_due_lost += (uint32_t)rest;
            }
            s_due_lost += lost;
            bm_hal_lock_exit(l);
            sl_due_emit();
            return;
        }
    }
    if (lost) {
        /* ids were lost (a full queue): the waiters whose end is past run now */
        int64_t now = bm_hal_now_us();
        uint32_t cap = zend_hash_num_elements(&s_waiters), k = 0;
        uint32_t *late = cap ? emalloc(cap * sizeof *late) : NULL;
        zend_ulong id;
        sl_waiter_t *w;
        ZEND_HASH_FOREACH_NUM_KEY_PTR(&s_waiters, id, w) {
            if (w->due_us <= now) {
                late[k++] = (uint32_t)id;
            }
        } ZEND_HASH_FOREACH_END();
        bool left = zend_hash_num_elements(&s_waiters) > k;
        for (uint32_t i = 0; i < k && !EG(exception); i++) {
            due_one(late[i]);
        }
        if (late) {
            efree(late);
        }
        if (left) {
            /* some are not due yet: they run at a later drain (the next HoldDue) */
            bm_hal_lock_enter(l);
            s_due_lost++;
            bm_hal_lock_exit(l);
        }
    }
}

void sl_ctl_due_stop(void)
{
    bm_hal_lock_t *l = __atomic_load_n(&s_due_lock, __ATOMIC_ACQUIRE);
    if (l) {
        bm_hal_lock_enter(l);
        s_due_n = 0;
        s_due_lost = 0;
        bm_hal_lock_exit(l);
    }
    if (!s_waiters_ready) {
        return;
    }
    while (zend_hash_num_elements(&s_waiters) > 0) {
        zend_ulong id = 0;
        zend_hash_internal_pointer_reset(&s_waiters);
        zend_hash_get_current_key(&s_waiters, NULL, &id);
        sl_waiter_t *w = waiter_take((uint32_t)id);
        if (!w) {
            break;
        }
        w->fn(w->obj, (uint32_t)id, true);
        waiter_free(w);
    }
    zend_hash_destroy(&s_waiters);   /* request memory: made again by the next wait */
    s_waiters_ready = false;
}

/* ---- touch and notifications ---- */

/* The method lcname of o's class unless it is Control's own (empty): NULL then. */
static zend_function *user_method(sl_obj_t *o, const char *lcname, size_t len)
{
    zend_function *fn = zend_hash_str_find_ptr(&o->std.ce->function_table, lcname, len);
    if (!fn || (fn->type == ZEND_INTERNAL_FUNCTION && fn->common.scope == sl_control_ce)) {
        return NULL;
    }
    return fn;
}

static bool call_pressed(sl_obj_t *o, bool down)
{
    zend_function *fn = user_method(o, "pressed", sizeof("pressed") - 1);
    if (fn) {
        zval arg;
        ZVAL_BOOL(&arg, down);
        zend_call_known_instance_method_with_1_params(fn, &o->std, NULL, &arg);
    }
    return !EG(exception);
}

bool sl_ctl_touch(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly)
{
    sl_ctl_t *c = o->ctl;
    if (kind == SL_TOUCH_PRESS) {
        c->down = true;
        c->owned = false;
        if (!call_pressed(o, true)) {
            return false;
        }
    } else if ((kind == SL_TOUCH_RELEASE || kind == SL_TOUCH_CANCEL) && c->down) {
        c->down = false;
        if (!call_pressed(o, false)) {
            return false;
        }
    }
    c = o->ctl;   /* the object is alive (deliver() holds it): the same record */
    if (kind == SL_TOUCH_PRESS && c && !c->down) {
        return true;   /* pressed(true) ended the gesture (a nested Cancel): nothing to start */
    }
    if (c && c->ops && c->ops->touch) {
        return c->ops->touch(o, kind, p, lx, ly);
    }
    return true;
}

bool sl_ctl_notify(sl_obj_t *o, zval *value)
{
    zend_function *fn = user_method(o, "onchange", sizeof("onchange") - 1);
    if (fn) {
        zend_call_known_instance_method_with_1_params(fn, &o->std, NULL, value);
        if (EG(exception)) {
            return false;
        }
    }
    if (o->ctl && Z_TYPE(o->ctl->changed) == IS_OBJECT) {
        zval cb, rv;
        ZVAL_COPY(&cb, &o->ctl->changed);   /* kept alive through the call (changed(null) inside it) */
        ZVAL_UNDEF(&rv);
        call_user_function(NULL, NULL, &cb, &rv, 1, value);
        zval_ptr_dtor(&rv);
        zval_ptr_dtor(&cb);
    }
    return !EG(exception);
}

/* ---- the properties of Slate's controls ---- */

static zend_property_info *ctl_prop_info(const sl_obj_t *o, zend_string *name)
{
    if (!o->ctl || !o->ctl->ops || !s_ce[o->base]) {
        return NULL;
    }
    zend_property_info *info = zend_hash_find_ptr(&o->std.ce->properties_info, name);
    if (!info || info->ce != s_ce[o->base] || (info->flags & ZEND_ACC_STATIC)) {
        return NULL;
    }
    if (s_ce[o->base]->parent && zend_hash_exists(&s_ce[o->base]->parent->properties_info, name)) {
        return NULL;   /* an Element property the class redeclares (Toggle::$hitPad): the elements' */
    }
    return info;
}

bool sl_ctl_is_prop(const sl_obj_t *o, zend_string *name)
{
    return ctl_prop_info(o, name) != NULL;
}

zval *sl_ctl_write(sl_obj_t *o, zend_string *name, zval *value)
{
    zend_property_info *info = ctl_prop_info(o, name);
    zval *slot = OBJ_PROP(&o->std, info->offset);
    zval old;
    ZVAL_COPY(&old, slot);
    zend_std_write_property(&o->std, name, value, NULL);
    if (EG(exception)) {
        zval_ptr_dtor(&old);
        return &EG(error_zval);
    }
    if (o->ctl && o->ctl->ops->write && !o->ctl->ops->write(o, ZSTR_VAL(info->name), slot, &old)) {
        zval tmp;
        ZVAL_COPY_VALUE(&tmp, slot);
        ZVAL_COPY_VALUE(slot, &old);
        zval_ptr_dtor(&tmp);
        return &EG(error_zval);
    }
    zval_ptr_dtor(&old);
    return slot;
}

/* ---- Control ---- */

PHP_METHOD(SlateControl, __construct)
{
    zval *x = NULL, *y = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    if (!sl_ctl_need_slate(sl_obj(obj))) {
        RETURN_THROWS();
    }
    if (x) {
        obj->handlers->write_property(obj, s_x_name, x, NULL);
    }
    if (y && !EG(exception)) {
        obj->handlers->write_property(obj, s_y_name, y, NULL);
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
}

PHP_METHOD(SlateControl, onChange)
{
    zval *v;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(v)
    ZEND_PARSE_PARAMETERS_END();
    (void)v;
}

PHP_METHOD(SlateControl, changed)
{
    zval *fn = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(fn, zend_ce_closure)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (o->ctl) {
        zval old;
        ZVAL_COPY_VALUE(&old, &o->ctl->changed);
        if (fn) {
            ZVAL_COPY(&o->ctl->changed, fn);
        } else {
            ZVAL_UNDEF(&o->ctl->changed);
        }
        zval_ptr_dtor(&old);   /* after: it may be running */
    }
    RETURN_OBJ_COPY(&o->std);
}

PHP_METHOD(SlateControl, notify)
{
    zval *v;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(v)
    ZEND_PARSE_PARAMETERS_END();
    sl_anim_flush();
    if (!sl_ctl_notify(sl_obj(Z_OBJ_P(ZEND_THIS)), v)) {
        RETURN_THROWS();
    }
}

PHP_METHOD(SlateControl, pressed)
{
    bool down;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(down)
    ZEND_PARSE_PARAMETERS_END();
    (void)down;
}

/* pressed() of Slate's controls (each class declares it, so a subclass's parent::pressed() draws the look). */
PHP_METHOD(SlateControl, builtinPressed)
{
    bool down;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(down)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (o->ctl && o->ctl->ops && o->ctl->ops->pressed) {
        sl_anim_flush();
        o->ctl->ops->pressed(o, down);
    }
}

/* add() and remove(): a user control's (and a Dialog's) own children; Slate's other controls refuse. */
static bool children_args(sl_obj_t *o, zval *args, uint32_t argc)
{
    if (o->base != SL_CONTROL && o->base != SL_DIALOG) {
        zend_throw_error(NULL, "%s: a control builds its own parts", ZSTR_VAL(o->std.ce->name));
        return false;
    }
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_OBJECT || !instanceof_function(Z_OBJCE(args[i]), sl_element_ce)) {
            zend_argument_type_error(i + 1, "must be of type Baremetal\\Slate\\Element, %s given",
                                     zend_zval_value_name(&args[i]));
            return false;
        }
    }
    return true;
}

PHP_METHOD(SlateControl, add)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (!children_args(o, args, argc)) {
        RETURN_THROWS();
    }
    sl_children_add(o, args, argc);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(&o->std);
}

PHP_METHOD(SlateControl, remove)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (!children_args(o, args, argc)) {
        RETURN_THROWS();
    }
    sl_children_remove(o, args, argc);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(&o->std);
}

#define T_NUM (MAY_BE_LONG | MAY_BE_DOUBLE)

ZEND_BEGIN_ARG_INFO_EX(arginfo_control_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_control_value, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_control_changed, 0, 1, IS_STATIC, 0)
    ZEND_ARG_OBJ_INFO(0, fn, Closure, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_control_pressed, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, down, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_control_children, 0, 0, IS_STATIC, 0)
    ZEND_ARG_VARIADIC_OBJ_INFO(0, e, Baremetal\\Slate\\Element, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry control_methods[] = {
    PHP_ME(SlateControl, __construct, arginfo_control_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateControl, onChange, arginfo_control_value, ZEND_ACC_PUBLIC)
    PHP_ME(SlateControl, changed, arginfo_control_changed, ZEND_ACC_PUBLIC)
    PHP_ME(SlateControl, notify, arginfo_control_value, ZEND_ACC_PROTECTED)
    PHP_ME(SlateControl, pressed, arginfo_control_pressed, ZEND_ACC_PROTECTED)
    PHP_ME(SlateControl, add, arginfo_control_children, ZEND_ACC_PUBLIC)
    PHP_ME(SlateControl, remove, arginfo_control_children, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void sl_ctl_class(zend_class_entry *ce, int base, const sl_ctl_ops_t *ops)
{
    s_ce[base] = ce;
    s_ops[base] = ops;
    sl_elements_control(base, ce);
}

void sl_controls_register(void)
{
    s_x_name = zend_string_init_interned("x", 1, 1);
    s_y_name = zend_string_init_interned("y", 1, 1);
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Control", control_methods);
    sl_control_ce = zend_register_internal_class_ex(&ce, sl_group_ce);
    sl_control_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | ZEND_ACC_NOT_SERIALIZABLE;
    zval dv;
    /* a control takes the touch: $touchable true by default (redeclared: Element's slot) */
    ZVAL_TRUE(&dv);
    zend_declare_typed_property(sl_control_ce, zend_string_init_interned("touchable", sizeof("touchable") - 1, 1), &dv,
                                ZEND_ACC_PUBLIC, NULL, (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0));
    ZVAL_TRUE(&dv);
    zend_property_info *pi = zend_declare_typed_property(
        sl_control_ce, zend_string_init_interned("enabled", sizeof("enabled") - 1, 1), &dv, ZEND_ACC_PUBLIC, NULL,
        (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0));
    s_enabled_off = pi->offset;
    ZVAL_UNDEF(&dv);
    static const char theme_cls[] = "Baremetal\\Slate\\Theme";
    zend_string *theme_name = zend_string_init_interned(theme_cls, sizeof theme_cls - 1, 1);
    pi = zend_declare_typed_property(sl_control_ce, zend_string_init_interned("theme", sizeof("theme") - 1, 1), &dv,
                                     ZEND_ACC_PROTECTED | ZEND_ACC_READONLY, NULL,
                                     (zend_type)ZEND_TYPE_INIT_CLASS(theme_name, 0, 0));
    s_theme_off = pi->offset;
    sl_elements_control(SL_CONTROL, sl_control_ce);

    sl_button_register();
    sl_toggle_register();
    sl_choice_register();
    sl_row_register();
    sl_dialog_register();
    sl_progress_register();
}

#endif
