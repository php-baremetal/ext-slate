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
 * The PHP layer of Slate (module `slate`, namespace Baremetal\Slate), internal to the package.
 *
 * Elements. Every element object (and every Scene, whose root is a group) carries an id, assigned at creation,
 * and keeps its properties in the object's own declared property slots: reading a property is a plain PHP read.
 * A write goes through the object handlers (php_slate_elements.c): the value is type-checked by PHP, validated
 * here (\ValueError naming the property and its limit), and, when the element lives in the engine, becomes a
 * SET command. Nothing reaches the engine before an element is "realized": when its scene is shown, or when it is
 * added to a realized group, Slate sends NEW, every property and the ADD of each child. An element is realized in
 * one engine generation (one Slate); a later Slate realizes it again when it shows it.
 *
 * Commands. They accumulate in the engine's pending batch while PHP runs, and are delivered to the compositor
 * when the PHP task returns to the event loop (ext-events' bm_events_on_idle(): after each delivered event in the
 * event-driven runtime, after each loop() in init-loop), by Slate::show() and by Slate::commit().
 *
 * Lifetime. A group (or a scene) holds a reference on each child; the active Slate holds the shown scene, the
 * display and the touch device. Freeing a realized element sends FREE (a group's children stay alive under their
 * own ids). Destroying the Slate (or the end of the request) removes the TE hook, stops the engine and gives the
 * panel back.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "php.h"
#include "engine.h"

extern zend_module_entry slate_module_entry;

/* The element classes and the scene (sl_obj_t.base). */
typedef enum {
    SL_GROUP = 0,
    SL_ROUNDRECT,
    SL_CIRCLE,
    SL_ARC,
    SL_LINE,
    SL_TEXT,
    SL_CANVAS,
    SL_IMAGE,
    SL_SCROLL,      /* Scroll (a Group made a scroll: php_slate_scroll.c) */
    SL_SCENE,
    SL_BASE_COUNT
} sl_base_t;

/* An element or a scene. */
typedef struct sl_obj sl_obj_t;
struct sl_obj {
    uint32_t id;
    uint32_t gen;           /* the engine generation it is realized in; 0: none */
    uint8_t base;           /* sl_base_t; SL_BASE_COUNT for a user class that extends Element directly */
    uint8_t kind;           /* slate_el_kind_t (a scene: GROUP) */
    bool has_children;      /* a group or a scene: children is initialised */
    bool built;             /* a scene: build() was called */
    sl_obj_t *parent;       /* the group or scene it belongs to (which holds a reference on it); NULL: none */
    HashTable children;     /* zvals of the children, bottom to top */
    uint16_t *px;           /* a canvas: cw x ch RGB565 native, emalloc */
    int cw, ch;
    int dx0, dy0, dx1, dy1; /* a canvas: the rectangle changed since the last dirty() (dx0 >= dx1: none) */
    struct slate_image *im; /* an Image: its image in the store (a reference held); NULL: none */
    uint16_t tweens;        /* its running tweens (php_slate_anim.c: each holds a reference on it) */
    bool animated;          /* a tween or ease() touched it: every write is sent, even of the value it holds (the
                               screen may show another one: a stopped tween, a remove()) */
    uint8_t ease;           /* ease(): the curve (slate_ease_t), the properties (bit 1u << slate_prop_t; 0: none) and
                               the time, sent again when it is realized */
    uint32_t ease_mask;
    int32_t ease_ms;
    struct sl_scroll *sc;   /* a Scroll: its PHP-side state (php_slate_scroll.c, emalloc); NULL otherwise */
    struct sl_touch_cbs *tcb; /* its on() callbacks (php_slate_touch.c, emalloc); NULL: none set yet */
    bool touch_noted;       /* in the id -> element map of the touch (php_slate_touch.c) */
    zend_object std;
};

static inline sl_obj_t *sl_obj(zend_object *o)
{
    return (sl_obj_t *)((char *)o - XtOffsetOf(sl_obj_t, std));
}

/* The engine of the running Slate (one at a time) and its generation; engine NULL when no Slate runs. */
typedef struct {
    slate_engine_t *engine;
    uint32_t gen;
    zend_object *slate;     /* the running Slate object (not referenced) */
} sl_state_t;
extern sl_state_t sl_g;

/* Class entries. */
extern zend_class_entry *sl_slate_ce, *sl_scene_ce, *sl_element_ce, *sl_group_ce, *sl_roundrect_ce, *sl_circle_ce,
    *sl_arc_ce, *sl_line_ce, *sl_text_ce, *sl_canvas_ce, *sl_caps_ce, *sl_align_ce, *sl_gradient_ce,
    *sl_linear_ce, *sl_radial_ce, *sl_shadow_ce, *sl_image_ce, *sl_tween_ce, *sl_tween_done_ce, *sl_ease_ce, *sl_transition_ce,
    *sl_direction_ce, *sl_transition_done_ce, *sl_scroll_ce, *sl_scroll_axis_ce, *sl_scroll_ended_ce,
    *sl_touch_ce, *sl_touch_type_ce, *sl_hold_due_ce;

/* php_slate.c: pushes a command into the pending batch of the running engine (nothing without one). Throws
   \RuntimeException and returns false when the engine has failed (slate_engine_error()) or refused the command
   (memory, pixels). */
bool sl_push(const slate_cmd_t *c);
/* Throws \RuntimeException with the engine's sticky error, if any; false then. */
bool sl_check(void);

/* php_slate_elements.c */
void sl_elements_register(void);
/* Sends NEW, the properties and the children of o (and of its subtree) to the running engine, once per
   generation. false with an exception. */
bool sl_realize(sl_obj_t *o);
/* Group and Scene: add() and remove() of the children in args. */
void sl_children_add(sl_obj_t *o, zval *args, uint32_t argc);
void sl_children_remove(sl_obj_t *o, zval *args, uint32_t argc);
/* Object handlers shared by the elements and the scene (create_object of the base classes). */
zend_object *sl_obj_create(zend_class_entry *ce);
/* Whether o lives in the running engine (realized in its generation). */
bool sl_realized(const sl_obj_t *o);
/* animate() and then(): checks every property of props (animatable for o's class, not a gradient now or as the
   target, the value as a write of it would check it, the rules between properties) and throws \ValueError (false,
   nothing changed) on the first refusal; else writes the values into o's properties (the final values that reading
   them gives; with back, the yoyo of an odd repeat ends where it started: they stay as they are) and fills
   spec->props and spec->n. */
bool sl_anim_collect(sl_obj_t *o, HashTable *props, bool back, slate_tween_spec_t *spec);

/* A property slot of o by name (NULL: o has no such Slate property), its number (0 when absent) and whether it is
   true. */
zval *sl_slot(sl_obj_t *o, const char *name);
double sl_slot_num(sl_obj_t *o, const char *name);
bool sl_slot_true(sl_obj_t *o, const char *name);

/* Gives the Scene class (registered by php_slate_scene.c) the element handlers. */
void sl_elements_scene(zend_class_entry *scene_ce);

/* php_slate_scene.c */
void sl_scene_register(void);

/* php_slate_effects.c: Caps, Align, Gradient, LinearGradient, RadialGradient, Shadow. */
void sl_effects_register(void);
/* The C paint of a Gradient object (stops owned by the object: copied by the command buffer at push); false when
   it has no stops (a user subclass of Gradient, or one whose constructor never ran). */
bool sl_gradient_paint(zend_object *g, slate_paint_t *out, slate_paint_grad_t *grad);
/* A Shadow object's values (blur already converted to the box radius); obj NULL: none. */
slate_value_t sl_shadow_value(zend_object *obj);
/* CSS blur (px) to the box-blur radius: round(blur / 3), at least 1 when blur > 0. */
int sl_blur_radius(zend_long blur);
/* The throw of an effect left out of the build: "<effect> is not built in: add it to slate-effects in
   composer.json". */
void sl_not_built(const char *effect);
/* 0xRRGGBB (opaque) or 0xAARRGGBB as an int, or as an integral float (a 32-bit PHP turns literals above
   0x7FFFFFFF into floats): false when v is not a colour. */
bool sl_color_of(const zval *v, uint32_t *argb);

/* php_slate_canvas.c: the methods of Canvas (its constructor comes from php_slate_elements.c). */
const zend_function_entry *sl_canvas_method_table(void);
/* Gives the canvas a w x h buffer, cleared to black (dirty emptied). false (exception) without memory. */
bool sl_canvas_resize(sl_obj_t *o, int w, int h);
/* Sends the pixels of the rectangle to the engine (realized canvases only). */
bool sl_canvas_send(sl_obj_t *o, int x0, int y0, int x1, int y1);

/* php_slate_images.c: Image (its methods and its image), Images, Preload, the events ImageLoaded, ImageFailed,
   PreloadProgress, PreloadDone and the enum ImageError; the store's listener. */
void sl_images_register(void);
const zend_function_entry *sl_image_method_table(void);
/* After the Image class is registered with its readonly properties w, h, loaded: their slots. */
void sl_image_class(zend_class_entry *ce);
/* A new Image object: w = h = 0, loaded = false. */
void sl_image_init(sl_obj_t *o);
/* Checks a value of Image::$src: false with the \ValueError of the Global rules (absolute, .sli or .png in any
   case, png built). */
bool sl_image_src_ok(const zval *v);
/* Image::$src was set to path: the image of the path (a reference), w, h and loaded from its state now, and an
   ImageFailed when the store gives it back failed already. false with an exception (no memory, no event source). */
bool sl_image_bind(sl_obj_t *o, zend_string *path);
/* The Image goes: its reference is dropped. */
void sl_image_unbind(sl_obj_t *o);
/* Whether the image of o failed (a write of the same $src tries again). */
bool sl_image_failed(const sl_obj_t *o);
/* The engine the store's listener kicks (NULL: none; waits until no listener uses the old one). PHP task. */
void sl_images_engine(slate_engine_t *e);
/* The end of the request: images in flight dropped, the events and the registries gone. */
void sl_images_rshutdown(void);
/* The images' part of Slate's idle hook (plain C). */
void sl_images_idle(void);

/* php_slate_scroll.c: Scroll (its touch(), scrollTo() and the PHP side of its position), the enum ScrollAxis and the
   event ScrollEnded. The properties of Scroll are php_slate_elements.c's (they call the functions below). */
/* ScrollAxis, registered before the Scroll class (php_slate_elements.c registers Scroll with this method table). */
void sl_scroll_axis_register(void);
const zend_function_entry *sl_scroll_method_table(void);
/* ScrollEnded (after Scroll and ext-events' Event). */
void sl_scroll_register(void);
/* The slate_scroll_axis_t of a ScrollAxis case. */
uint8_t sl_scroll_axis_of(const zval *v);
/* A new Scroll object (sl_obj_create) gets its state; its free gives it back. */
void sl_scroll_init(sl_obj_t *o);
void sl_scroll_free(sl_obj_t *o);
/* Realization (sl_realize): the CONFIG right after its NEW; the JUMP to its position after its children. false with
   an exception. */
bool sl_scroll_realize_config(sl_obj_t *o);
bool sl_scroll_realize_position(sl_obj_t *o);
/* Reading $scrollX/$scrollY: their slots get the position drawn (when the engine has applied the last write). */
void sl_scroll_sync(sl_obj_t *o);
/* A write of $scrollX/$scrollY (the slots hold the new values): a JUMP of a realized scroll, which stops its motion
   and its scrollTo(). false with an exception. */
bool sl_scroll_jumped(sl_obj_t *o);
/* A write of $axis, $bounce, $snapX, $snapY, $snapMax or $indicators that changed it: the CONFIG of a realized
   scroll (setup: one of the first five, which stops its scrollTo()). false with an exception. */
bool sl_scroll_configured(sl_obj_t *o, bool setup);
/* The scroll ends of the engine (its tween listener, the engine's task): one ScrollEnded each, for the Scrolls still
   alive. */
void sl_scroll_pull(slate_engine_t *e);
/* The end of the request: the event set up goes. */
void sl_scroll_rshutdown(void);
/* The touch routing drives a Scroll (php_slate_touch.c), with the state machine of touch() (its threshold, its catch
   of a motion) and the time of each touch event; touch() leaves such a touch alone. Every bool: false with an
   exception. */
struct sl_touch;
/* Moving now (a drag, an inertia, a snap, a spring, a scrollTo): a press there is a catch. */
bool sl_scroll_moving(sl_obj_t *o);
/* A touch that hit it, pressed at *t. */
bool sl_scroll_take(sl_obj_t *o, const struct sl_touch *t);
/* A touch handed over to it: pressed at *press, and *t its first DRAG (the engine anchors there: no jump). */
bool sl_scroll_take_drag(sl_obj_t *o, const struct sl_touch *press, const struct sl_touch *t);
/* The touch is the scroll's for good (a drag started, or its press caught a motion): no hand-off any more. */
bool sl_scroll_owns(const sl_obj_t *o);
/* A Moved (released false) or the Released of the touch it was given. */
bool sl_scroll_event(sl_obj_t *o, const struct sl_touch *t, bool released);
/* The touch it was given ends without its Released: send, a CANCEL (a release without velocity) while the scroll
   is on the screen; else only the state is cleared (the Slate stopped). */
bool sl_scroll_drop(sl_obj_t *o, bool send);

/* php_slate_anim.c: Tween, TweenDone, Ease; the engine's tween listener and the TweenDone events. */
void sl_anim_register(void);
/* ms >= 1, delay >= 0, repeat >= -1 (each at most INT32_MAX): false with the \ValueError of the Global rules. */
bool sl_anim_times(zend_long ms, zend_long delay, zend_long repeat);
/* The slate_ease_t of an Ease case (NULL: OutCubic). */
uint8_t sl_ease_of(zend_object *ease);
/* A new Tween into rv for spec on el (spec->tween is set here), after pred (then(); NULL: none). A realized el gets
   a TWEEN command; one that is not drawn yet has its values already: the Tween is done at once (its TweenDone
   follows), or stopped for repeat -1. false with an exception (rv undefined). */
bool sl_tween_make(zval *rv, sl_obj_t *el, slate_tween_spec_t *spec, zend_object *pred);
/* A new Tween into rv that is done at once (its TweenDone follows), for a change that took no time (scrollTo() with
   ms 0, or of a Scroll not drawn). false with an exception (rv undefined). */
bool sl_tween_instant(zval *rv, sl_obj_t *el);
/* The running Tween id was stopped by the engine without a TWEEN_STOP (a scroll's PRESS, JUMP or new set-up stops its
   scrollTo()): marked stopped ($done false, wait() false, no TweenDone). Nothing for a Tween that is not running. */
void sl_tween_silenced(uint32_t id);
/* Whether the Tween id runs (not done, stopped or pending; its end not reported by the engine yet). */
bool sl_tween_running(uint32_t id);
/* c was detached (a REMOVE pushed, or its parent freed) while it has tweens: checked at the next delivery
   (sl_deliver); without a running Slate they stop at once. */
void sl_anim_removed(sl_obj_t *c);
/* o was just realized: its pending tweens (made while it was not drawn) are sent, the values before them first.
   false with an exception. */
bool sl_anim_realized(sl_obj_t *o);
/* A write of prop on o: its pending tweens no longer move prop. */
void sl_anim_written(sl_obj_t *o, slate_prop_t prop);
/* The animations' part of Slate's idle hook (plain C). */
void sl_anim_idle(void);
/* Hands the pending batch to the compositor: first the tweens of the elements removed since the last delivery and
   not put back are marked stopped (the engine stops them without a done). Plain C (the idle hook calls it). */
void sl_deliver(void);
/* Releases what plain C left to release (elements of tweens that ended or stopped). PHP calls only. */
void sl_anim_flush(void);
/* The engine the tween listener drains (NULL: the Slate stopped: every running tween is marked stopped). */
void sl_anim_engine(slate_engine_t *e);
/* The end of the request: the Tweens and the events dropped. */
void sl_anim_rshutdown(void);

/* php_slate_anim.c, transitions: Transition, Direction, TransitionDone; the records of the transitions asked by
   show(), from the show() to the engine's end of each. */
/* The transition of a Transition object into *out; false (out all zeros: at once) for NULL. */
bool sl_transition_of(zend_object *t, slate_trans_t *out);
/* The first transition of the request: the TransitionDone event, its watch, the idle hook. false with an exception. */
bool sl_trans_ready(void);
/* A SHOW of to with a transition was pushed, from from (NULL: no scene shown): recorded (references) until its end. */
void sl_trans_add(zend_object *from, zend_object *to);
/* Before such a SHOW: the memory its end will need on the engine's task (taken now, so no end is lost). false with
   an exception. */
bool sl_trans_reserve(void);
/* A show() during a transition: onLeave() now of the scenes the running ones leave (their TransitionDone follows when
   the engine ends them). false with an exception. */
bool sl_trans_leave(void);
/* onLeave() of the transitions that ended and whose TransitionDone was not delivered (a full pool, init-loop); the
   records go. PHP calls only (show(), commit(), the watch of TransitionDone). false with an exception. */
bool sl_trans_settle(void);
/* A transition was asked and has not ended (the ends already reported applied first): the touch is ignored. Plain
   C. */
bool sl_trans_running(void);
/* The transitions' part of Slate's idle hook (plain C). */
void sl_trans_idle(void);

/* php_slate_touch.c: the touch on elements (Touch, TouchType, the routing of the touch events of the Slate's device to
   the elements and to Scene::onTouch(), Hold, Cancel). Element's properties $touchable, $hitPad, $holdMs and its
   methods on(), off(), onPress()..onCancel() are php_slate_elements.c's (they call the functions below). */

/* The six kinds of a touch, in TouchType's order (also the order of the methods and of the callbacks). */
typedef enum { SL_TOUCH_PRESS, SL_TOUCH_MOVE, SL_TOUCH_RELEASE, SL_TOUCH_TAP, SL_TOUCH_HOLD, SL_TOUCH_CANCEL,
               SL_TOUCH_KINDS } sl_touch_kind_t;

/* A point of the finger: screen pixels and the time of its ext-input event (µs, bm_hal_now_us()'s clock). */
typedef struct sl_touch {
    int x, y;
    int64_t t_us;
} sl_touch_t;

/* Who the touch under way belongs to (from its Pressed to its Released). */
typedef enum {
    SL_OWN_NONE,      /* no touch, or one whose Pressed Slate did not see: its events go to Scene::onTouch() */
    SL_OWN_SCENE,     /* its Pressed hit nothing: Scene::onTouch() gets its raw events */
    SL_OWN_ELEMENT,   /* an element captured it (sl_touch_state_t.el) */
    SL_OWN_SCROLL,    /* a Scroll has it (sl_touch_state_t.el): hit, or handed over past 8 px */
    SL_OWN_DEAD,      /* cancelled: the rest of it reaches nobody */
} sl_touch_owner_t;

/* The state of the touch under way (one finger). */
typedef struct {
    uint8_t owner;           /* sl_touch_owner_t */
    zend_object *el;         /* SL_OWN_ELEMENT: the captured element, SL_OWN_SCROLL: the Scroll (a reference); NULL
                                otherwise */
    uint32_t serial;         /* +1 at each capture (a handler can tell its capture ended) */
    sl_touch_t press;        /* the Pressed point and time */
    sl_touch_t last;         /* the last point seen (a Cancel or a Hold carries it) */
    int ox, oy;              /* the captured element's origin on the screen, as last found in the hit table */
    bool past;               /* the finger went more than 8 px from the Pressed point */
    bool held;               /* Hold was delivered */
    bool cancel;             /* a Cancel waits for the handler under way to return */
    bool taken;              /* an element or a Scroll took the touch (also after its end, until the next Pressed):
                                its late Gesture is not the scene's */
} sl_touch_state_t;
extern sl_touch_state_t sl_touch_st;

void sl_touch_register(void);
/* The Slate starts with a touch device: the hit table given to the engine, the Hold timer and its watch. false with an
   exception. */
bool sl_touch_start(slate_engine_t *e);
/* The Slate stops: the touch under way goes without a Cancel, the table and the watch go. */
void sl_touch_stop(slate_engine_t *e);
/* A touch event of the Slate's device while scene is shown and no transition runs: to the elements, or to
   Scene::onTouch(). */
void sl_touch_route(zend_object *event, zend_object *scene);
/* A touch event of the Slate's device ignored (a transition runs): a Pressed starts a touch Slate did not see, whose
   later events (and Gesture) go to Scene::onTouch() once the transition is over. */
void sl_touch_unseen(zend_object *event);
/* show() of another scene, and the start of a transition: the captured element gets Cancel now. */
void sl_touch_cancel_now(void);
/* After a change that can end the capture (remove(), $visible, $touchable, off()): when the captured element is no
   longer in the scene shown, visible with its ancestors and touchable, it gets Cancel (after the handler under way,
   else now). */
void sl_touch_recheck(void);
/* Starts a capture of el at t (ox, oy: its origin, from its hit table entry): the Hold timer, then Press. false with
   an exception. */
bool sl_touch_capture(zend_object *el, const sl_touch_t *t, int ox, int oy);

/* Elements: whether o has a callback; on()/off() of kind (fn NULL: off); the callbacks' GC and free. */
bool sl_touch_has_cb(const sl_obj_t *o);
void sl_touch_set_cb(sl_obj_t *o, int kind, zval *fn);
void sl_touch_gc(sl_obj_t *o, zend_get_gc_buffer *buf);
/* o can get touches now ($touchable or a callback): noted in the id -> element map. */
void sl_touch_note(sl_obj_t *o);
/* o goes (its free): out of the map, its callbacks freed. */
void sl_touch_forget(sl_obj_t *o);
/* The touchable kind index of a TouchType case. */
int sl_touch_kind_of(zend_object *type);
/* The end of the request. */
void sl_touch_rshutdown(void);

/* php_slate.c: Slate's one hook of ext-events' idle table (bm_events_on_idle), shared by its users: the running
   Slate (its delivery), the images, the animations and the transitions (their lost events). Set while any user is on; false when the
   table is full. PHP task. */
#define SL_IDLE_SLATE 1u
#define SL_IDLE_IMAGES 2u
#define SL_IDLE_ANIM 4u
#define SL_IDLE_TRANS 8u
bool sl_idle_use(unsigned user, bool on);

/* php_slate.c: the scene the running Slate shows (NULL: none, or no Slate). */
zend_object *sl_shown_scene(void);

/* The short name of a class ("Gauge" for App\Gauge). */
const char *sl_short_name(zend_class_entry *ce);
