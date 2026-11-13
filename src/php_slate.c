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
 * The module `slate` and Baremetal\Slate\Slate: the object that takes a display's panel (display_claim()), starts
 * the compositor task (engine.h) on its band path, paces it on the TE pulses (display_vsync_hook()) when the
 * panel has them, shows one Scene at a time (at once, or with a transition: php_slate_anim.c, transitions) and
 * forwards the touch events of its Touch device to that scene (none during a transition).
 *
 * Delivery. The commands of the PHP task are handed to the compositor when the PHP task returns to the event loop
 * (bm_events_on_idle() of ext-events: after each delivered event in the event-driven runtime, after each loop()
 * in init-loop), at show() and at commit(). So the changes made by one listener are one batch: one frame.
 *
 * One Slate at a time: the compositor and its rasterizers are not reentrant (engine.h). A second Slate, on any
 * panel, throws \LogicException while the first one lives.
 *
 * Kept scenes. $keepScenes (int >= 0, default 2) and $keptBudget (?int KB, default null) are the limits of the
 * caches the scenes left keep (engine.h, Kept scenes: slate_engine_set_keep, at start and at every write); a scene
 * left keeps them unless its Scene::$keepCache is false (read at the show() that leaves it: the SHOW's keep).
 */
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "php.h"
#include "ext/standard/info.h"
#include "ext/spl/spl_exceptions.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "zend_smart_str.h"

#include "bm_board.h"
#include "bm_input.h"
#include "display_panel.h"
#include "event_bus_php.h"
#include "php_slate.h"
#include "slate_config.h"

sl_state_t sl_g;
zend_class_entry *sl_slate_ce;

static zend_object_handlers slate_handlers;

#define TOUCH_CLASSES 4

typedef struct {
    zend_object *display, *touch, *source, *scene;
    slate_engine_t *engine;
    bool hook;                                   /* display_vsync_hook() set */
    zend_class_entry *touch_ce[TOUCH_CLASSES];   /* the touch event classes watched */
    zend_object std;
} sl_slate_t;

static sl_slate_t *slate_of(zend_object *o)
{
    return (sl_slate_t *)((char *)o - XtOffsetOf(sl_slate_t, std));
}

static zend_property_info *s_keep_pi, *s_budget_pi;   /* Slate::$keepScenes, Slate::$keptBudget */

/* The limits of the kept scenes to the engine: $keepScenes, and $keptBudget in bytes (null: none of its own; 0 KB:
   nothing kept). */
static void sl_keep_apply(sl_slate_t *s)
{
    if (!s->engine) {
        return;
    }
    zval *k = OBJ_PROP(&s->std, s_keep_pi->offset), *b = OBJ_PROP(&s->std, s_budget_pi->offset);
    zend_long scenes = Z_TYPE_P(k) == IS_LONG ? Z_LVAL_P(k) : 2;
    size_t bytes = 0;
    if (Z_TYPE_P(b) == IS_LONG) {
        zend_ulong kb = (zend_ulong)Z_LVAL_P(b);
        if (kb == 0) {
            scenes = 0;
        }
        bytes = kb > SIZE_MAX / 1024 ? SIZE_MAX : (size_t)kb * 1024;
    }
    slate_engine_set_keep(s->engine, scenes > INT_MAX ? INT_MAX : (int)scenes, bytes);
}

/* Scene::$keepCache of scene (true unless it is false). */
static bool scene_keeps(zend_object *scene)
{
    zval rv;
    zval *v = zend_read_property(sl_scene_ce, scene, "keepCache", sizeof("keepCache") - 1, 1, &rv);
    return !v || Z_TYPE_P(v) != IS_FALSE;
}

/* The engine the TE hook wakes: set once it runs, cleared before it stops (the hook is removed first). */
static slate_engine_t *s_vsync_engine;

/* ---- the engine glue ---- */

bool sl_check(void)
{
    if (!sl_g.engine) {
        return true;
    }
    const char *err = slate_engine_error(sl_g.engine);
    if (err) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Slate: %s", err);
        return false;
    }
    return true;
}

bool sl_push(const slate_cmd_t *c)
{
    if (!sl_g.engine) {
        return true;
    }
    if (!sl_check()) {
        return false;
    }
    switch (slate_cmd_push(slate_engine_cmds(sl_g.engine), c)) {
    case SLATE_CMD_OK:
        return true;
    case SLATE_CMD_NO_MEM:
        zend_throw_exception(spl_ce_RuntimeException, "Slate: out of memory for the changes of the scene", 0);
        return false;
    case SLATE_CMD_BAD_PIXELS:
        zend_throw_exception(spl_ce_RuntimeException,
                             "Slate: the canvas pixels were refused (an empty rectangle, or more than 2 MB)", 0);
        return false;
    case SLATE_CMD_BAD_ARG:   /* a TWEEN or EASE without its spec, a SHOW with a bad transition: the PHP layer sends
                                 them whole and checked */
        zend_throw_exception(spl_ce_RuntimeException, "Slate: an animation command was refused", 0);
        return false;
    }
    return false;
}

/* The TE interrupt (display_vsync_hook()): ISR-safe, it only wakes the engine. */
static void sl_vsync(void *arg)
{
    (void)arg;
    slate_engine_t *e = __atomic_load_n(&s_vsync_engine, __ATOMIC_ACQUIRE);
    if (e) {
        slate_engine_vsync(e);
    }
}

/* The claimed panel's news (display_claim_ex(), PHP task): asleep, no frames; awake, frames again; its picture
   lost (wake re-initialised it), the whole screen again. */
static void sl_panel(void *arg, display_panel_event_t event)
{
    (void)arg;
    if (!sl_g.engine) {
        return;
    }
    switch (event) {
    case DISPLAY_PANEL_SLEEP:
        slate_engine_pause(sl_g.engine, true);
        break;
    case DISPLAY_PANEL_WAKE:
        slate_engine_pause(sl_g.engine, false);
        break;
    case DISPLAY_PANEL_LOST:
        slate_engine_redraw(sl_g.engine);
        break;
    }
}

/* Back in the event loop (bm_events_on_idle()): the batch of the listener that just returned goes; the images, the
   animations and the transitions catch up their lost events. Plain C. One hook for all of Slate's users (ext-events keeps 4). */
static unsigned s_idle_users;

static void sl_idle(void *arg)
{
    (void)arg;
    unsigned u = s_idle_users;
    if ((u & SL_IDLE_SLATE) && sl_g.engine) {
        sl_deliver();
    }
    if (u & SL_IDLE_IMAGES) {
        sl_images_idle();
    }
    if (u & SL_IDLE_ANIM) {
        sl_anim_idle();
    }
    if (u & SL_IDLE_TRANS) {
        sl_trans_idle();
    }
}

bool sl_idle_use(unsigned user, bool on)
{
    unsigned was = s_idle_users;
    unsigned now = on ? was | user : was & ~user;
    if (!was && now && !bm_events_on_idle(sl_idle, NULL)) {
        return false;
    }
    s_idle_users = now;
    if (was && !now) {
        bm_events_off_idle(sl_idle, NULL);
    }
    return true;
}

static void call0(zend_object *obj, const char *lcname)
{
    zend_function *fn = zend_hash_str_find_ptr(&obj->ce->function_table, lcname, strlen(lcname));
    if (fn) {
        zend_call_known_instance_method_with_0_params(fn, obj, NULL);
    }
}

/* A touch event of the bus: the active scene's onTouch(), when the event comes from this Slate's Touch; none
   while a transition runs (from its show() to its end). */
static void sl_touch(zend_object *event, void *arg)
{
    sl_slate_t *s = arg;
    if (!s->engine || !s->scene || !s->touch || sl_trans_running()) {
        return;
    }
    zval rv;
    zval *dev = zend_read_property(event->ce, event, "device", sizeof("device") - 1, 1, &rv);
    if (!dev || Z_TYPE_P(dev) != IS_OBJECT || Z_OBJ_P(dev) != s->touch) {
        return;
    }
    zend_object *scene = s->scene;
    zend_function *fn = zend_hash_str_find_ptr(&scene->ce->function_table, "ontouch", sizeof("ontouch") - 1);
    if (!fn) {
        return;
    }
    GC_ADDREF(scene);   /* onTouch() may show another scene */
    zval ev;
    ZVAL_OBJ(&ev, event);
    zend_call_known_instance_method_with_1_params(fn, scene, NULL, &ev);
    OBJ_RELEASE(scene);
}

static const char *const touch_classes[TOUCH_CLASSES] = {
    "baremetal\\input\\pressed", "baremetal\\input\\moved", "baremetal\\input\\released", "baremetal\\input\\gesture",
};

/* Stops what s started: TE hook, engine, panel claim, hooks of ext-events; drops its references. */
static void sl_stop(sl_slate_t *s)
{
    if (s->engine) {
        for (int i = 0; i < TOUCH_CLASSES; i++) {
            if (s->touch_ce[i]) {
                bm_events_unwatch(s->touch_ce[i], sl_touch, s);
                s->touch_ce[i] = NULL;
            }
        }
        sl_idle_use(SL_IDLE_SLATE, false);
        if (s->hook) {
            display_vsync_hook(s->display, NULL, NULL);   /* before the stop: no pulse uses the engine after */
            s->hook = false;
        }
        __atomic_store_n(&s_vsync_engine, NULL, __ATOMIC_RELEASE);
        sl_images_engine(NULL);   /* no image listener kicks the engine after */
        slate_engine_t *e = s->engine;
        s->engine = NULL;
        sl_g.engine = NULL;
        sl_g.slate = NULL;
        if (!slate_engine_stop(e)) {
            /* read after the stop, which gives the reason it failed; e is not freed when it returns false */
            const char *err = slate_engine_error(e);
            php_error_docref(NULL, E_WARNING, "Slate: the compositor did not stop cleanly (%s); its memory stays",
                             err ? err : "the panel did not finish a band");
        }
        sl_anim_engine(NULL);   /* after the stop: the listener ran on the engine's task; no tween will end */
        display_release(s->display);
    }
    zend_object *o;
    if ((o = s->scene)) {
        s->scene = NULL;
        OBJ_RELEASE(o);
    }
    if ((o = s->source)) {
        s->source = NULL;
        OBJ_RELEASE(o);
    }
    if ((o = s->touch)) {
        s->touch = NULL;
        OBJ_RELEASE(o);
    }
    if ((o = s->display)) {
        s->display = NULL;
        OBJ_RELEASE(o);
    }
}

/* new Slate($display, $touch): false with an exception. */
static bool sl_start(sl_slate_t *s, zend_object *display, zend_object *touch)
{
    if (s->engine || s->display) {
        zend_throw_error(NULL, "Slate is already constructed");
        return false;
    }
    if (sl_g.slate) {
        sl_slate_t *other = slate_of(sl_g.slate);
        zend_throw_exception_ex(spl_ce_LogicException, 0,
                                "Slate: a Slate already drives %s; only one Slate can run at a time",
                                other->display ? display_panel_name(other->display) : "a panel");
        return false;
    }
    const display_panel_ops_t *ops;
    void *panel;
    display_panel_of(display, &ops, &panel);
    if (!ops) {
        zend_throw_error(NULL, "%s is not initialised", display_panel_name(display));
        return false;
    }
    if (!ops->band) {
        zend_throw_exception_ex(spl_ce_LogicException, 0, "Slate: %s has no band path", display_panel_name(display));
        return false;
    }
    if (!display_claim_ex(display, "Slate", sl_panel, NULL)) {
        zend_throw_exception_ex(spl_ce_LogicException, 0,
                                "Slate: cannot take the panel of %s: another renderer drives it, or its flush did "
                                "not end in time", display_panel_name(display));
        return false;
    }
    bool has_te = display_vsync_hook(display, sl_vsync, NULL);
    slate_engine_t *e = slate_engine_start(ops->band, panel, ops->width, ops->height, ops->refresh_hz, has_te);
    if (!e) {
        if (has_te) {
            display_vsync_hook(display, NULL, NULL);
        }
        display_release(display);
        zend_throw_exception(spl_ce_RuntimeException,
                             "Slate: cannot start the compositor (no memory for its buffers, or no task)", 0);
        return false;
    }
    __atomic_store_n(&s_vsync_engine, e, __ATOMIC_RELEASE);
    sl_images_engine(e);   /* an image that finishes loading wakes it */
    sl_anim_engine(e);     /* the tween listener (set at MINIT, copied by the start) drains its dones */
    GC_ADDREF(display);
    s->display = display;
    s->engine = e;
    s->hook = has_te;
    sl_g.engine = e;
    sl_g.slate = &s->std;
    if (++sl_g.gen == 0) {
        sl_g.gen = 1;
    }
    sl_idle_use(SL_IDLE_SLATE, true);
    sl_keep_apply(s);
    if (touch) {
        GC_ADDREF(touch);
        s->touch = touch;
        zval rv;
        ZVAL_UNDEF(&rv);
        zend_call_method_with_0_params(touch, touch->ce, NULL, "events", &rv);
        if (EG(exception)) {
            zval_ptr_dtor(&rv);
            sl_stop(s);
            return false;
        }
        if (Z_TYPE(rv) == IS_OBJECT) {
            s->source = Z_OBJ(rv);   /* the reference of the return value */
        } else {
            zval_ptr_dtor(&rv);
        }
        for (int i = 0; i < TOUCH_CLASSES; i++) {
            zend_class_entry *ce = zend_hash_str_find_ptr(CG(class_table), touch_classes[i], strlen(touch_classes[i]));
            if (ce && bm_events_watch(ce, sl_touch, s)) {
                s->touch_ce[i] = ce;
            }
        }
    }
    return true;
}

zend_object *sl_shown_scene(void)
{
    return sl_g.slate ? slate_of(sl_g.slate)->scene : NULL;
}

static sl_slate_t *running(zval *this_ptr)
{
    sl_slate_t *s = slate_of(Z_OBJ_P(this_ptr));
    if (!s->engine) {
        zend_throw_error(NULL, "Slate is not running (it was not constructed, or it stopped)");
        return NULL;
    }
    return s;
}

/* ---- Slate ---- */

ZEND_BEGIN_ARG_INFO_EX(arginfo_slate_ctor, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, display, Baremetal\\Output\\Display, 0)
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, touch, Baremetal\\Input\\Touch, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(SlateSlate, __construct)
{
    zend_object *display, *touch = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJ_OF_CLASS(display, display_ce())
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJ_OF_CLASS_OR_NULL(touch, bm_input_touch_ce())
    ZEND_PARSE_PARAMETERS_END();
    if (!sl_start(slate_of(Z_OBJ_P(ZEND_THIS)), display, touch)) {
        RETURN_THROWS();
    }
}

/* The class among the loaded ones that implements iface and declares a public static onboard(); *n is how many
   do (their names in names). */
static zend_class_entry *onboard_class(zend_class_entry *iface, int *n, smart_str *names)
{
    zend_class_entry *found = NULL, *ce;
    zend_string *key;
    *n = 0;
    ZEND_HASH_FOREACH_STR_KEY_PTR(CG(class_table), key, ce) {
        if (!key || !zend_string_equals_ci(key, ce->name)) {
            continue;   /* an alias (class_alias()) or a runtime key: the class is counted under its own name */
        }
        if (ce->type == ZEND_USER_CLASS && !(ce->ce_flags & ZEND_ACC_LINKED)) {
            continue;
        }
        if (ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_ENUM | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS
                            | ZEND_ACC_IMPLICIT_ABSTRACT_CLASS)) {
            continue;
        }
        if (!iface || !instanceof_function(ce, iface)) {
            continue;
        }
        zend_function *fn = zend_hash_str_find_ptr(&ce->function_table, "onboard", sizeof("onboard") - 1);
        if (!fn || fn->common.scope != ce || !(fn->common.fn_flags & ZEND_ACC_STATIC)
            || !(fn->common.fn_flags & ZEND_ACC_PUBLIC)) {
            continue;
        }
        if (*n > 0) {
            smart_str_appends(names, ", ");
        }
        smart_str_append(names, ce->name);
        found = ce;
        (*n)++;
    } ZEND_HASH_FOREACH_END();
    smart_str_0(names);
    return found;
}

static zend_object *call_onboard(zend_class_entry *ce)
{
    zend_function *fn = zend_hash_str_find_ptr(&ce->function_table, "onboard", sizeof("onboard") - 1);
    zval rv;
    ZVAL_UNDEF(&rv);
    zend_call_known_function(fn, NULL, ce, &rv, 0, NULL, NULL);
    if (EG(exception)) {
        zval_ptr_dtor(&rv);
        return NULL;
    }
    if (Z_TYPE(rv) != IS_OBJECT) {
        zval_ptr_dtor(&rv);
        zend_throw_error(NULL, "Slate::onboard(): %s::onboard() did not return an object", ZSTR_VAL(ce->name));
        return NULL;
    }
    return Z_OBJ(rv);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_slate_onboard, 0, 0, Baremetal\\Slate\\Slate, 0)
ZEND_END_ARG_INFO()

/* Slate::onboard(): the display (and the touch device) of the board, from the loaded driver classes that have an
   onboard() method: exactly one display driver when the board has a `display` block; a touch driver when the
   board has a `touch` block and exactly one is loaded. */
PHP_METHOD(SlateSlate, onboard)
{
    ZEND_PARSE_PARAMETERS_NONE();
    const char *hint = "build it with new Slate($display, $touch)";
    if (sl_g.slate) {   /* before the drivers' onboard(): they would open the panel Slate drives */
        sl_slate_t *other = slate_of(sl_g.slate);
        zend_throw_exception_ex(spl_ce_LogicException, 0,
                                "Slate: a Slate already drives %s; only one Slate can run at a time",
                                other->display ? display_panel_name(other->display) : "a panel");
        RETURN_THROWS();
    }
    if (!bm_board_type("display")) {
        zend_throw_exception_ex(spl_ce_LogicException, 0, "Slate::onboard(): the board has no display block; %s",
                                hint);
        RETURN_THROWS();
    }
    int n;
    smart_str names = {0};
    zend_class_entry *dce = onboard_class(display_ce(), &n, &names);
    if (n != 1) {
        if (n == 0) {
            zend_throw_exception_ex(spl_ce_LogicException, 0,
                                    "Slate::onboard(): no display driver with an onboard() method is loaded; %s",
                                    hint);
        } else {
            zend_throw_exception_ex(spl_ce_LogicException, 0,
                                    "Slate::onboard(): %d display drivers have an onboard() method (%s); %s", n,
                                    ZSTR_VAL(names.s), hint);
        }
        smart_str_free(&names);
        RETURN_THROWS();
    }
    smart_str_free(&names);
    zend_class_entry *tce = NULL;
    if (bm_board_type("touch")) {
        smart_str tnames = {0};
        tce = onboard_class(bm_input_touch_ce(), &n, &tnames);
        if (n > 1) {
            zend_throw_exception_ex(spl_ce_LogicException, 0,
                                    "Slate::onboard(): %d touch drivers have an onboard() method (%s); %s", n,
                                    ZSTR_VAL(tnames.s), hint);
            smart_str_free(&tnames);
            RETURN_THROWS();
        }
        smart_str_free(&tnames);
    }
    zend_object *display = call_onboard(dce);
    if (!display) {
        RETURN_THROWS();
    }
    zend_object *touch = tce ? call_onboard(tce) : NULL;
    if (tce && !touch) {
        OBJ_RELEASE(display);
        RETURN_THROWS();
    }
    object_init_ex(return_value, sl_slate_ce);
    bool ok = sl_start(slate_of(Z_OBJ_P(return_value)), display, touch);
    OBJ_RELEASE(display);
    if (touch) {
        OBJ_RELEASE(touch);
    }
    if (!ok) {
        zval_ptr_dtor(return_value);
        ZVAL_UNDEF(return_value);
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_slate_show, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, s, Baremetal\\Slate\\Scene, 0)
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, t, Baremetal\\Slate\\Transition, 1, "null")
ZEND_END_ARG_INFO()

/* show(): nothing for the scene shown (also the one a running transition goes to). A running transition ends first:
   onLeave() of the scene it leaves now (its TransitionDone follows). Without a transition: onLeave() of the scene
   shown until now, build() the first time, onEnter(). With one: build() the first time, onEnter() at once, and at
   its end onLeave() of the scene left and TransitionDone (sl_trans_*). The change is delivered at once (with what
   onEnter() did). */
PHP_METHOD(SlateSlate, show)
{
    zend_object *scene, *tobj = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJ_OF_CLASS(scene, sl_scene_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJ_OF_CLASS_OR_NULL(tobj, sl_transition_ce)
    ZEND_PARSE_PARAMETERS_END();
    sl_slate_t *s = running(ZEND_THIS);
    if (!s || !sl_check()) {
        RETURN_THROWS();
    }
    sl_anim_flush();
    if (!sl_trans_settle() || !s->engine) {
        RETURN_THROWS();
    }
    if (s->scene == scene) {
        sl_deliver();
        return;
    }
    slate_trans_t tr;
    bool trans = sl_transition_of(tobj, &tr);
    if (trans && (!sl_trans_ready() || !sl_trans_reserve())) {
        RETURN_THROWS();
    }
    if (!sl_trans_leave() || !s->engine) {
        RETURN_THROWS();
    }
    if (s->scene && !trans) {
        call0(s->scene, "onleave");
        if (EG(exception) || !s->engine) {
            RETURN_THROWS();
        }
    }
    sl_obj_t *so = sl_obj(scene);
    if (!so->built) {
        so->built = true;
        call0(scene, "build");
        if (EG(exception) || !s->engine) {
            /* a build() that failed: the scene is emptied and built again by the next show(); the scene shown
               until now stays (without a transition its onLeave() already ran) */
            so->built = false;
            zval *zv;
            ZEND_HASH_FOREACH_VAL(&so->children, zv) {
                sl_obj(Z_OBJ_P(zv))->parent = NULL;
            } ZEND_HASH_FOREACH_END();
            zend_hash_clean(&so->children);
            RETURN_THROWS();
        }
    }
    if (!sl_realize(so)) {
        RETURN_THROWS();
    }
    slate_cmd_t c = { .kind = SLATE_CMD_SHOW, .id = so->id, .data = sl_short_name(scene->ce),
                      .v = slate_v_color(0xFF000000u), .tr = tr, .keep = s->scene && scene_keeps(s->scene) };
    if (!sl_push(&c)) {
        RETURN_THROWS();
    }
    zend_object *old = s->scene;
    if (trans) {
        sl_trans_add(old, scene);   /* onLeave() of old at its end */
    }
    GC_ADDREF(scene);
    s->scene = scene;
    if (old) {
        OBJ_RELEASE(old);
    }
    call0(scene, "onenter");
    if (s->engine) {
        sl_deliver();
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_slate_scene, 0, 0, Baremetal\\Slate\\Scene, 1)
ZEND_END_ARG_INFO()

PHP_METHOD(SlateSlate, scene)
{
    ZEND_PARSE_PARAMETERS_NONE();
    sl_slate_t *s = slate_of(Z_OBJ_P(ZEND_THIS));
    if (!s->scene) {
        RETURN_NULL();
    }
    RETURN_OBJ_COPY(s->scene);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_slate_commit, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

/* commit(): delivers the pending changes now, for a script that does not return to an event loop. */
PHP_METHOD(SlateSlate, commit)
{
    ZEND_PARSE_PARAMETERS_NONE();
    sl_slate_t *s = running(ZEND_THIS);
    if (!s || !sl_check()) {
        RETURN_THROWS();
    }
    sl_anim_flush();
    if (!sl_trans_settle()) {
        RETURN_THROWS();
    }
    if (s->engine) {   /* an onLeave() may have dropped the Slate */
        sl_deliver();
    }
}

static const zend_function_entry slate_methods[] = {
    PHP_ME(SlateSlate, __construct, arginfo_slate_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateSlate, onboard, arginfo_slate_onboard, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(SlateSlate, show, arginfo_slate_show, ZEND_ACC_PUBLIC)
    PHP_ME(SlateSlate, scene, arginfo_slate_scene, ZEND_ACC_PUBLIC)
    PHP_ME(SlateSlate, commit, arginfo_slate_commit, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static zend_object *slate_create(zend_class_entry *ce)
{
    sl_slate_t *s = zend_object_alloc(sizeof(sl_slate_t), ce);
    memset(s, 0, XtOffsetOf(sl_slate_t, std));
    zend_object_std_init(&s->std, ce);
    object_properties_init(&s->std, ce);
    s->std.handlers = &slate_handlers;
    return &s->std;
}

static void slate_dtor(zend_object *obj)
{
    sl_stop(slate_of(obj));
    zend_objects_destroy_object(obj);
}

static void slate_free(zend_object *obj)
{
    sl_stop(slate_of(obj));
    zend_object_std_dtor(obj);
}

static HashTable *slate_gc(zend_object *obj, zval **table, int *n)
{
    sl_slate_t *s = slate_of(obj);
    zend_get_gc_buffer *buf = zend_get_gc_buffer_create();
    zend_object *refs[] = { s->display, s->touch, s->source, s->scene };
    for (size_t i = 0; i < sizeof refs / sizeof refs[0]; i++) {
        if (refs[i]) {
            zend_get_gc_buffer_add_obj(buf, refs[i]);
        }
    }
    zend_get_gc_buffer_use(buf, table, n);
    return zend_std_get_properties(obj);   /* $keepScenes, $keptBudget (no object in them) and dynamic ones */
}

/* $keepScenes and $keptBudget: PHP's typed write, then the range (the old value back on a refusal), then the engine.
   No cache slot: every write comes here. */
static zval *slate_write_property(zend_object *obj, zend_string *name, zval *value, void **cache_slot)
{
    bool scenes = zend_string_equals(name, s_keep_pi->name);
    if (!scenes && !zend_string_equals(name, s_budget_pi->name)) {
        return zend_std_write_property(obj, name, value, cache_slot);
    }
    zval *slot = OBJ_PROP(obj, (scenes ? s_keep_pi : s_budget_pi)->offset);
    zval old;
    ZVAL_COPY_VALUE(&old, slot);   /* an int or null */
    zend_std_write_property(obj, name, value, NULL);
    if (EG(exception)) {
        return &EG(error_zval);
    }
    if (Z_TYPE_P(slot) == IS_LONG && Z_LVAL_P(slot) < 0) {
        ZVAL_COPY_VALUE(slot, &old);
        zend_value_error("%s must be 0 or more", scenes ? "keepScenes" : "keptBudget");
        return &EG(error_zval);
    }
    sl_keep_apply(slate_of(obj));
    return slot;
}

static bool slate_own(zend_string *name)
{
    return zend_string_equals(name, s_keep_pi->name) || zend_string_equals(name, s_budget_pi->name);
}

/* No reference to them (it would change them without the check and without reaching the engine). */
static zval *slate_read_property(zend_object *obj, zend_string *name, int type, void **cache_slot, zval *rv)
{
    if (type != BP_VAR_R && type != BP_VAR_IS && slate_own(name)) {
        zend_throw_error(NULL, "Cannot take a reference to %s::$%s", ZSTR_VAL(obj->ce->name), ZSTR_VAL(name));
        return &EG(uninitialized_zval);
    }
    return zend_std_read_property(obj, name, type, cache_slot, rv);
}

static zval *slate_get_property_ptr_ptr(zend_object *obj, zend_string *name, int type, void **cache_slot)
{
    if (slate_own(name)) {
        return NULL;   /* compound assignments and ++ go through read + write */
    }
    return zend_std_get_property_ptr_ptr(obj, name, type, cache_slot);
}

static void slate_unset_property(zend_object *obj, zend_string *name, void **cache_slot)
{
    if (slate_own(name)) {
        zend_throw_error(NULL, "Cannot unset %s::$%s", ZSTR_VAL(obj->ce->name), ZSTR_VAL(name));
        return;
    }
    zend_std_unset_property(obj, name, cache_slot);
}

/* ---- module ---- */

PHP_MINIT_FUNCTION(slate)
{
    (void)type;
    (void)module_number;
    sl_effects_register();
    sl_elements_register();
    sl_scene_register();
    sl_images_register();
    sl_anim_register();
    sl_scroll_register();

    memcpy(&slate_handlers, zend_get_std_object_handlers(), sizeof slate_handlers);
    slate_handlers.offset = XtOffsetOf(sl_slate_t, std);
    slate_handlers.dtor_obj = slate_dtor;
    slate_handlers.free_obj = slate_free;
    slate_handlers.clone_obj = NULL;
    slate_handlers.get_gc = slate_gc;
    slate_handlers.write_property = slate_write_property;
    slate_handlers.read_property = slate_read_property;
    slate_handlers.get_property_ptr_ptr = slate_get_property_ptr_ptr;
    slate_handlers.unset_property = slate_unset_property;

    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Slate", slate_methods);
    sl_slate_ce = zend_register_internal_class(&ce);
    sl_slate_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    sl_slate_ce->create_object = slate_create;
    zval dv;
    ZVAL_LONG(&dv, 2);
    s_keep_pi = zend_declare_typed_property(sl_slate_ce, zend_string_init_interned("keepScenes", 10, 1), &dv,
                                            ZEND_ACC_PUBLIC, NULL, (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));
    ZVAL_NULL(&dv);
    s_budget_pi = zend_declare_typed_property(sl_slate_ce, zend_string_init_interned("keptBudget", 10, 1), &dv,
                                              ZEND_ACC_PUBLIC, NULL, (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 1, 0));
    return SUCCESS;
}

/* The end of the request (or of the program): the running Slate stops, even when no destructor ran (a fatal
   error). Before the modules it depends on shut down (ext-display, ext-input, ext-events come later). */
PHP_RSHUTDOWN_FUNCTION(slate)
{
    (void)type;
    (void)module_number;
    if (sl_g.slate) {
        sl_stop(slate_of(sl_g.slate));
    }
    sl_images_rshutdown();
    sl_anim_rshutdown();
    sl_scroll_rshutdown();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(slate)
{
    (void)zend_module;
    php_info_print_table_start();
    php_info_print_table_row(2, "Slate", "enabled");
    php_info_print_table_row(2, "gradients", SLATE_HAS_GRADIENTS ? "built" : "not built");
    php_info_print_table_row(2, "shadows", SLATE_HAS_SHADOWS ? "built" : "not built");
    php_info_print_table_row(2, "blur", SLATE_HAS_BLUR ? "built" : "not built");
    php_info_print_table_row(2, "png", SLATE_HAS_PNG ? "built" : "not built");
    php_info_print_table_row(2, "debug", SLATE_DEBUG ? "on" : "off");
    php_info_print_table_end();
}

static const zend_module_dep slate_deps[] = {
    ZEND_MOD_REQUIRED("events")
    ZEND_MOD_REQUIRED("display")
    ZEND_MOD_REQUIRED("input")
    ZEND_MOD_END
};

zend_module_entry slate_module_entry = {
    STANDARD_MODULE_HEADER_EX,
    NULL,
    slate_deps,
    "slate",
    NULL,
    PHP_MINIT(slate),
    NULL,
    NULL,
    PHP_RSHUTDOWN(slate),
    PHP_MINFO(slate),
    "1.2.0",
    STANDARD_MODULE_PROPERTIES
};
