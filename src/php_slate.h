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
    *sl_linear_ce, *sl_radial_ce, *sl_shadow_ce, *sl_image_ce;

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

/* The short name of a class ("Gauge" for App\Gauge). */
const char *sl_short_name(zend_class_entry *ce);
