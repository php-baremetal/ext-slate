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
#include "slate_config.h"

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
    /* the controls (php_slate_control.c): engine groups; SL_CONTROL is a user class that extends Control directly,
       the others Slate's controls (and the user classes that extend them) */
    SL_CONTROL,
    SL_BUTTON,
    SL_TOGGLE,
    SL_SLIDER,
    SL_CHOICE,
    SL_SETTING_ROW,
    SL_DIALOG,
    SL_PROGRESS_RING,
    SL_PROGRESS_BAR,
    SL_SPINNER,
    SL_STEP_LIST,
    SL_BASE_COUNT
} sl_base_t;

static inline bool sl_base_control(int b)
{
    return b >= SL_CONTROL && b <= SL_STEP_LIST;
}

/* A base whose objects have children (o->children). */
static inline bool sl_base_group(int b)
{
    return b == SL_GROUP || b == SL_SCROLL || b == SL_SCENE || sl_base_control(b);
}

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
#if SLATE_HAS_CONTROLS
    struct sl_ctl *ctl;     /* a control (php_slate_control.c, emalloc); NULL otherwise */
#endif
    zend_object std;
};
/* The control record of an element (NULL: not a control; always NULL without the controls). */
#if SLATE_HAS_CONTROLS
#define SL_CTL(o) ((o)->ctl)
#else
#define SL_CTL(o) ((struct sl_ctl *)NULL)
#endif

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

/* The box of c in its parent's coordinates (geometry from its slots: RoundRect and Canvas x..x+w; Circle x+-r; Arc
   x+-(r+width/2); Line its ends +- width/2; Text slate_text_bbox; Image x..x+$w; a Slate control its size laid out
   (a ProgressRing or a Spinner centred on its x, y); a
   Group, Scroll or user Control its visible children (a Scroll: its view; a Group with clip: its clip box)); false when
   c is not visible. */
bool sl_obj_box(sl_obj_t *c, int *x0, int *y0, int *x1, int *y1);
/* The box of o's visible children from (0, 0), in o's coordinates: the far corner of their boxes, at least 0. */
void sl_obj_extent(sl_obj_t *o, int *w, int *h);
/* Moves the child c of o to the top (last in the order): REMOVE + ADD in the same batch when both are realized (a
   move for the engine, engine.h). */
void sl_children_raise(sl_obj_t *o, sl_obj_t *c);
/* Writes a number into o's property name through the element's handler (checked, a SET when o is realized). */
bool sl_write_num(sl_obj_t *o, const char *name, int v);
/* Gives the Scene class (registered by php_slate_scene.c) the element handlers. */
void sl_elements_scene(zend_class_entry *scene_ce);
/* A control base class (php_slate_control.c): its objects are elements of that base (an engine GROUP). The first call
   (Control, SL_CONTROL) builds the property tables of the controls from it. */
void sl_elements_control(int base, zend_class_entry *ce);
/* The next element id (the elements' counter; never 0): an element's, or a part's of a control. */
uint32_t sl_id_new(void);
/* The font of the firmware named name (len bytes); NULL when there is none. */
const struct display_font_entry *sl_font_find(const char *name, size_t len);
/* The \ValueError of an unknown font: "<cls>::$<prop> must be a font of the firmware: unknown font '<name>'
   (available: ...)". */
void sl_font_error(const char *cls, const char *prop, zend_string *name);

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
/* The image of path from the store (a reference, given back with slate_image_put): a control's icon. It loads as an
   Image's does (ImageLoaded, ImageFailed). NULL with an exception (no memory, no event source). */
struct slate_image *sl_image_acquire(zend_string *path);
/* A control's icon path: absolute, ending in .sli or .png (in any case), png built. false with the \ValueError
   "<cls>::$<prop> must be an absolute path ending in .sli or .png" (prop "icons": "<cls>::$icons must hold absolute
   paths ending in .sli or .png"; or png's "is not built in"). */
bool sl_image_icon_ok(const char *cls, const char *prop, zend_string *path);
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
/* Internal tweens (a control's, the toast's): ids with SL_TWEEN_CTL, from a counter of their own (30 bits), so they
   never meet a Tween's (1..0x7FFFFFFF). pull() gives them no TweenDone; one with SL_TWEEN_CTL_CB as well goes to
   sl_ctl_due() at its end (and HoldDue wakes the PHP task). */
#define SL_TWEEN_CTL 0x80000000u      /* a control's own tween: no TweenDone */
#define SL_TWEEN_CTL_CB 0x40000000u   /* ... whose end the control wants (sl_ctl_due) */
/* A new internal id (cb: with SL_TWEEN_CTL_CB). 0 with an exception (no memory for the lock of the dones). */
uint32_t sl_tween_ctl_id(bool cb);

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
/* The Slate starts with a touch device: the hit table given to the engine, the Hold timer (the watch of HoldDue is
   sl_due_start()'s: started here when it is not yet). false with an exception. */
bool sl_touch_start(slate_engine_t *e);
/* The Slate stops: the touch under way goes without a Cancel, the table goes (then sl_due_stop()). */
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
/* Something now covers the whole scene (a Dialog opened): the touch under way ends. An element's or a Scroll's gets
   Cancel (after the handler under way, else now), a touch of the scene's reaches it no more. */
void sl_touch_cancel_held(void);
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
/* HoldDue: Slate's internal event for its timers (the Hold of a touch, the end of a control's animation): its source
   and its watch, set while a Slate runs with a touch device or with the controls built (touch: the message names the
   touch). false with an exception. sl_touch_start() finds them. */
bool sl_due_start(bool touch);
void sl_due_stop(void);
/* Queues one HoldDue (coalescing) while the watch is set. Any task (not an ISR). */
void sl_due_emit(void);

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
/* php_slate.c: the screen of the running Slate (false: none). */
bool sl_screen(int *w, int *h);

/* The short name of a class ("Gauge" for App\Gauge). */
const char *sl_short_name(zend_class_entry *ce);

/* ---- the theme and the controls (slate-controls; without it their classes are absent) ---- */

/* php_slate_theme.c: Theme, Size, Variant, Shape, ChoiceStyle. */
extern zend_class_entry *sl_theme_ce, *sl_size_ce, *sl_variant_ce, *sl_shape_ce, *sl_choice_style_ce;

/* A Theme object: its tokens for C (the PHP side reads its public readonly properties). */
typedef struct sl_theme {
    uint32_t c[14];          /* bg, surface, raised, well, border, borderStrong, text, textMuted, textDim, accent,
                                onAccent, danger, info, success: 0xRRGGBB */
    int radius, size_h[3];   /* small, medium, large */
    const struct display_font_entry *font, *font_small, *font_title, *font_mono, *font_display;   /* NULL: none */
    zend_object std;
} sl_theme_t;

enum { SL_T_BG, SL_T_SURFACE, SL_T_RAISED, SL_T_WELL, SL_T_BORDER, SL_T_BORDER_STRONG, SL_T_TEXT, SL_T_TEXT_MUTED,
       SL_T_TEXT_DIM, SL_T_ACCENT, SL_T_ON_ACCENT, SL_T_DANGER, SL_T_INFO, SL_T_SUCCESS };

static inline sl_theme_t *sl_theme_of(zend_object *o)
{
    return (sl_theme_t *)((char *)o - XtOffsetOf(sl_theme_t, std));
}

/* The enums, then Theme. */
void sl_theme_register(void);
/* A new Theme with the default tokens into rv, without checking the fonts (a font the firmware lacks is NULL): the
   theme a Slate starts with. */
void sl_theme_default(zval *rv);
/* The running Slate's theme (php_slate.c: Slate::$theme); NULL without a running Slate. */
zend_object *sl_theme_current(void);
/* The slate_ctl_size_t of a Size case; the index of a case of any enum among names (0 when not found). */
int sl_size_of(const zval *v);
int sl_enum_case(const zval *v, const char *const *names, int n);

/* php_slate_control.c: Control and the parts of the controls. A control is one PHP object (an engine GROUP); its
   parts (a background, a text, an icon...) are engine elements without a PHP object, made by the control's C code:
   ids of their own (sl_id_new), NEW when the control is realized, then the control's SETs (ops->realize), then the
   ADD of each part to its parent, before the control's PHP children (which go on top). A part's parent is the
   control's group or a GROUP part. */
typedef struct sl_part {
    uint32_t id;
    uint8_t kind;      /* slate_el_kind_t */
    int8_t parent;     /* the index of a GROUP part; -1: the control */
} sl_part_t;

typedef struct sl_ctl_ops sl_ctl_ops_t;

typedef struct sl_ctl {
    const sl_ctl_ops_t *ops;   /* NULL: a user class that extends Control directly */
    sl_part_t *parts;          /* emalloc */
    uint16_t nparts, cap;
    int16_t w, h;              /* laid out (Slate's controls); $w and $h read them */
    bool down;                 /* between Press and Release / Cancel */
    bool owned;                /* the touch is the control's (a Slider past 8 px) */
    zval changed;              /* the changed() callback; UNDEF: none */
    zval cb2;                  /* committed() of Slider, closed() of Dialog; UNDEF: none */
    void *st;                  /* the control's own state (emalloc), freed by ops->free */
} sl_ctl_t;

struct sl_ctl_ops {
    /* the SET (and EASE) of every part, after their NEW and before their ADD; false with an exception */
    bool (*realize)(sl_obj_t *o);
    /* a touch of kind (sl_touch_kind_t) at p, (lx, ly) local, before the method and the callback of that kind;
       false with an exception (NULL: none) */
    bool (*touch)(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly);
    /* the pressed look of a Slate control: Control::pressed() of it (NULL: none) */
    void (*pressed)(sl_obj_t *o, bool down);
    /* a property of the control's class was written (slot: the new value, PHP checked its type; old: the value
       before); false with an exception (the old value goes back) */
    bool (*write)(sl_obj_t *o, const char *name, zval *slot, zval *old);
    /* the engine parent of a PHP child (0: the control) (NULL: always the control) */
    uint32_t (*child_parent)(sl_obj_t *o, sl_obj_t *child);
    /* the zvals the state holds (NULL: none) */
    void (*gc)(sl_obj_t *o, zend_get_gc_buffer *buf);
    /* the state goes (NULL: efree of st) */
    void (*free)(sl_obj_t *o);
};

#if SLATE_HAS_CONTROLS
extern zend_class_entry *sl_control_ce;
/* Control, then each control class. */
void sl_controls_register(void);
/* A Slate control class of base (its ops): registered with the elements. */
void sl_ctl_class(zend_class_entry *ce, int base, const sl_ctl_ops_t *ops);
/* pressed() of Slate's controls: ops->pressed (a class lists it with ZEND_MALIAS(SlateControl, pressed,
   builtinPressed, <arginfo of pressed(bool $down): void>, ZEND_ACC_PROTECTED)). */
ZEND_METHOD(SlateControl, builtinPressed);
/* create_object of a control base: its ctl, and $theme from the running Slate. */
void sl_ctl_init(sl_obj_t *o);
/* The \Error "<C> needs a running Slate: create the Slate before its controls" without a running Slate; false then. */
bool sl_ctl_need_slate(sl_obj_t *o);
/* A part of kind under parent (-1: the control): its id reserved, its index (-1 with an exception: no memory). */
int sl_part_add(sl_obj_t *o, slate_el_kind_t kind, int parent);
/* A SET (an EASE) of part i, sent when the control is realized (else nothing: ops->realize sends the values then).
   false with an exception. */
bool sl_part_set(sl_obj_t *o, int i, slate_prop_t prop, slate_value_t v);
bool sl_part_ease(sl_obj_t *o, int i, uint32_t mask, int ms, slate_ease_t ease);
/* Realization (sl_realize, after the control's own properties): NEW of the parts, ops->realize, ADD of the parts. */
bool sl_ctl_realize(sl_obj_t *o);
/* The control goes (its free, before the FREE of its group): FREE of the parts (realized), ops->free, callbacks. */
void sl_ctl_free(sl_obj_t *o);
/* The engine parent of a PHP child of o. */
uint32_t sl_ctl_parent_of(sl_obj_t *o, sl_obj_t *child);
/* $enabled; the factor of its opacity (1, or SLATE_CTL_DISABLED when not enabled). */
bool sl_ctl_enabled(const sl_obj_t *o);
float sl_ctl_opacity(const sl_obj_t *o);
/* The touch hook (php_slate_touch.c, deliver()): Press calls pressed(true), Release and Cancel pressed(false) (the PHP
   method: an override wins), then ops->touch. false with an exception. */
bool sl_ctl_touch(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly);
/* Control::notify(): onChange($value), then the changed() callback (an exception stops there). false with an
   exception. */
bool sl_ctl_notify(sl_obj_t *o, zval *value);
/* Whether name is a property of the class of a Slate control (not in the elements' tables); its write. */
bool sl_ctl_is_prop(const sl_obj_t *o, zend_string *name);
zval *sl_ctl_write(sl_obj_t *o, zend_string *name, zval *value);   /* &EG(error_zval) on a refusal */
/* The callbacks and the state's zvals, for the GC. */
void sl_ctl_gc(sl_obj_t *o, zend_get_gc_buffer *buf);
/* The slot of $theme (protected readonly Theme of Control) of o; its theme (NULL when the slot is empty). */
sl_theme_t *sl_ctl_theme(sl_obj_t *o);
/* 0xFF000000 | rgb. */
static inline uint32_t sl_rgb(uint32_t rgb)
{
    return 0xFF000000u | (rgb & 0xFFFFFFu);
}
/* An internal tween of part (-1: the control's group) of a realized o: props (from ignored), ms, delay, ease, repeat,
   after a tween (0: none); cb: its end goes to the waiter of its id (sl_ctl_wait). *id: its id (0: o not realized,
   nothing sent). false with an exception. */
bool sl_part_tween(sl_obj_t *o, int part, const slate_tween_prop_t *props, int n, int ms, int delay,
                   slate_ease_t ease, int repeat, uint32_t after, bool cb, uint32_t *id);
/* TWEEN_STOP of an internal tween (0: nothing); to_end: its final values first. false with an exception. */
bool sl_part_tween_stop(uint32_t id, bool to_end);
/* PREPARE of part (-1: the control's group) of a realized o (cmd.h): its caches are built when the batch is applied,
   and the tweens pushed after it start once they are. Nothing when o is not realized. false with an exception. */
bool sl_part_prepare(sl_obj_t *o, int part);

/* The ends the controls wait for (internal tweens with SL_TWEEN_CTL_CB). sl_ctl_due(): any task (the tween listener):
   the id queued (32 at most: past that the newest is counted lost, and the waiters whose time is past are run at the
   next drain). sl_ctl_due_run(): the PHP task (the watch of HoldDue): each queued id runs its waiter. */
void sl_ctl_due(uint32_t id);
void sl_ctl_due_run(void);
/* A waiter: fn(obj, id, stopped) at the end of tween id (stopped: the Slate stopped, the tween will not end: no PHP
   call then); obj (NULL allowed) is held until then; ms: when the end is due from now (for a lost id). false with
   an exception (no memory for the queue's lock). */
typedef void (*sl_due_fn_t)(zend_object *obj, uint32_t id, bool stopped);
bool sl_ctl_wait(uint32_t id, zend_object *obj, sl_due_fn_t fn, int ms);
/* The waiter of id goes without being run (its obj released). */
void sl_ctl_unwait(uint32_t id);
/* The Slate stops: every waiter runs with stopped, the queue is emptied. */
void sl_ctl_due_stop(void);

/* php_slate_ctl_button.c */
void sl_button_register(void);
/* A Button's width given by its container (a Dialog): laid out again (w >= 1). false with an exception. */
bool sl_button_width(sl_obj_t *o, int w);
/* php_slate_ctl_toggle.c: Toggle and Slider */
void sl_toggle_register(void);
/* A Slider's track length given by its container (a SettingRow), w >= 0: laid out again. false with an exception. */
bool sl_slider_width(sl_obj_t *o, int w);
/* php_slate_ctl_choice.c */
void sl_choice_register(void);
/* php_slate_ctl_row.c: SettingRow */
void sl_row_register(void);
/* php_slate_ctl_progress.c: ProgressRing, ProgressBar, Spinner, StepList */
void sl_progress_register(void);
/* php_slate_ctl_dialog.c: Dialog, and what Scene does with dialogs. */
void sl_dialog_register(void);
extern zend_class_entry *sl_dialog_ce;
/* Scene::open(): d above the scene's children; false with an exception. */
bool sl_dialog_open(sl_obj_t *scene, sl_obj_t *d);
/* Scene::add() of a scene with open dialogs: they go back on top. */
void sl_dialog_raise(sl_obj_t *scene);
/* Whether o is an open Dialog (opened, its close() not ended). */
bool sl_dialog_is_open(const sl_obj_t *o);
/* Whether o is a Dialog whose close() animates (nothing in it takes a touch). */
bool sl_dialog_closing(const sl_obj_t *o);
/* The open dialog on top of scene (the last open one among its children whose $visible is true); NULL for none. */
sl_obj_t *sl_dialog_top(zend_object *scene);
/* $visible of the Dialog d set to false: a close under way ends now (removed, onClose(), closed()). false with an
   exception. */
bool sl_dialog_hidden(sl_obj_t *d);
/* Scene::remove() took the open dialog d out: it ends now (onClose(), closed()). false with an exception. */
bool sl_dialog_removed(sl_obj_t *d);
/* show() leaves scene: its dialogs that are closing end now (removed, onClose(), closed()). false with an exception. */
bool sl_dialog_leave(sl_obj_t *scene);
/* Whether c may go into a group or a scene by add(): not a Dialog (\Error "<C> is opened with Scene::open()"),
   unless Scene::open() adds it. false with the exception. */
bool sl_dialog_add_ok(sl_obj_t *c);
/* The lock of the due queue and the table of the waiters ready, so sl_ctl_wait() cannot fail. false with an
   exception. */
bool sl_ctl_due_ready(void);
#else
static inline void sl_ctl_init(sl_obj_t *o) { (void)o; }
static inline bool sl_ctl_realize(sl_obj_t *o) { (void)o; return true; }
static inline void sl_ctl_free(sl_obj_t *o) { (void)o; }
static inline uint32_t sl_ctl_parent_of(sl_obj_t *o, sl_obj_t *child) { (void)child; return o->id; }
static inline bool sl_ctl_enabled(const sl_obj_t *o) { (void)o; return true; }
static inline bool sl_dialog_closing(const sl_obj_t *o) { (void)o; return false; }
static inline sl_obj_t *sl_dialog_top(zend_object *scene) { (void)scene; return NULL; }
static inline float sl_ctl_opacity(const sl_obj_t *o) { (void)o; return 1.0f; }
static inline bool sl_ctl_touch(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly)
{
    (void)o; (void)kind; (void)p; (void)lx; (void)ly;
    return true;
}
static inline bool sl_ctl_is_prop(const sl_obj_t *o, zend_string *name) { (void)o; (void)name; return false; }
static inline zval *sl_ctl_write(sl_obj_t *o, zend_string *name, zval *value)
{
    (void)o; (void)name; (void)value;
    return &EG(error_zval);
}
static inline void sl_ctl_gc(sl_obj_t *o, zend_get_gc_buffer *buf) { (void)o; (void)buf; }
static inline void sl_ctl_due(uint32_t id) { (void)id; }
static inline void sl_ctl_due_run(void) {}
static inline void sl_ctl_due_stop(void) {}
#endif
