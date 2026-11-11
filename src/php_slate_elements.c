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
 * The elements: abstract Element (x, y, visible, opacity, set()), Group (add, remove), RoundRect, Circle, Arc,
 * Line, Text, Canvas (its drawing methods are in php_slate_canvas.c) and Image (its image, wait() and the events
 * are in php_slate_images.c). See php_slate.h for the model.
 *
 * Properties are declared, typed public properties. The object handlers route a write of a Slate property through
 * sl_assign(): PHP checks the type (and coerces, as for any typed property), the value is validated here
 * (\ValueError "<Class>::$<name> must be ..." and the old value stays), and a realized element sends a SET.
 * get_property_ptr_ptr refuses those properties, so `$el->x += 4` and `$el->x++` go through the write handler
 * too; unset() of one throws, and so does foreach by reference over an element (its references would change the
 * properties without validation, as a reference taken one by one would).
 */
#include <math.h>
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#if PHP_VERSION_ID >= 80400
#include "zend_property_hooks.h"
#endif
#include "zend_smart_str.h"
#include "ext/spl/spl_exceptions.h"

#include "display_font.h"
#include "php_slate.h"
#include "slate_config.h"

zend_class_entry *sl_element_ce, *sl_group_ce, *sl_roundrect_ce, *sl_circle_ce, *sl_arc_ce, *sl_line_ce,
    *sl_text_ce, *sl_canvas_ce, *sl_image_ce;

static zend_object_handlers sl_handlers;
static uint32_t s_next_id;

/* ---- property descriptors ---- */

typedef enum {
    PT_COORD,     /* int|float, -32767..32767 */
    PT_SIZE,      /* int|float, 0..32767 */
    PT_ANGLE,     /* int|float, degrees, finite */
    PT_OPACITY,   /* float, 0..1 */
    PT_BOOL,
    PT_COLOR,     /* int|float colour */
    PT_NCOLOR,    /* int|float|null colour (null: transparent) */
    PT_PAINT,     /* int|float|Gradient */
    PT_NPAINT,    /* int|float|Gradient|null */
    PT_TEXT,      /* string */
    PT_FONT,      /* string: a font of the firmware */
    PT_CAPS,      /* Caps */
    PT_ALIGN,     /* Align */
    PT_SHADOW,    /* ?Shadow */
    PT_BLUR,      /* int, CSS px 0..765 */
    PT_PIXELS,    /* int, 0..32767 */
    PT_CSIZE,     /* int, a canvas side 1..1024 */
    PT_SRC,       /* string: an image's path (php_slate_images.c checks it and holds the image) */
} sl_ptype_t;

typedef struct {
    const char *name;
    slate_prop_t prop;
    uint8_t type;   /* sl_ptype_t */
} sl_pdef_t;

typedef struct {
    const sl_pdef_t *def;
    zend_string *name;
    uint32_t offset;
} sl_prop_t;

#define PROPS_MAX 16

static const sl_pdef_t common_defs[] = {
    { "x", SLATE_PROP_X, PT_COORD },
    { "y", SLATE_PROP_Y, PT_COORD },
    { "visible", SLATE_PROP_VISIBLE, PT_BOOL },
    { "opacity", SLATE_PROP_OPACITY, PT_OPACITY },
};
static const sl_pdef_t group_defs[] = {
    { "w", SLATE_PROP_W, PT_SIZE },
    { "h", SLATE_PROP_H, PT_SIZE },
    { "clip", SLATE_PROP_CLIP, PT_BOOL },
    { "cache", SLATE_PROP_CACHE, PT_BOOL },
    { "blur", SLATE_PROP_BLUR, PT_BLUR },
    { "shadow", SLATE_PROP_SHADOW, PT_SHADOW },
};
static const sl_pdef_t roundrect_defs[] = {
    { "w", SLATE_PROP_W, PT_SIZE },
    { "h", SLATE_PROP_H, PT_SIZE },
    { "radius", SLATE_PROP_RADIUS, PT_SIZE },
    { "fill", SLATE_PROP_FILL, PT_NPAINT },
    { "border", SLATE_PROP_BORDER, PT_SIZE },
    { "borderColor", SLATE_PROP_BORDER_COLOR, PT_NCOLOR },
    { "shadow", SLATE_PROP_SHADOW, PT_SHADOW },
};
static const sl_pdef_t circle_defs[] = {
    { "r", SLATE_PROP_R, PT_SIZE },
    { "fill", SLATE_PROP_FILL, PT_NPAINT },
    { "border", SLATE_PROP_BORDER, PT_SIZE },
    { "borderColor", SLATE_PROP_BORDER_COLOR, PT_NCOLOR },
    { "shadow", SLATE_PROP_SHADOW, PT_SHADOW },
};
static const sl_pdef_t arc_defs[] = {
    { "r", SLATE_PROP_R, PT_SIZE },
    { "from", SLATE_PROP_FROM, PT_ANGLE },
    { "to", SLATE_PROP_TO, PT_ANGLE },
    { "width", SLATE_PROP_WIDTH, PT_SIZE },
    { "color", SLATE_PROP_COLOR, PT_PAINT },
    { "caps", SLATE_PROP_CAPS, PT_CAPS },
};
static const sl_pdef_t line_defs[] = {
    { "x1", SLATE_PROP_X1, PT_COORD },
    { "y1", SLATE_PROP_Y1, PT_COORD },
    { "width", SLATE_PROP_WIDTH, PT_SIZE },
    { "color", SLATE_PROP_COLOR, PT_COLOR },
    { "caps", SLATE_PROP_CAPS, PT_CAPS },
};
static const sl_pdef_t text_defs[] = {
    { "text", SLATE_PROP_TEXT, PT_TEXT },
    { "font", SLATE_PROP_FONT, PT_FONT },
    { "color", SLATE_PROP_COLOR, PT_COLOR },
    { "align", SLATE_PROP_ALIGN, PT_ALIGN },
    { "maxWidth", SLATE_PROP_MAX_WIDTH, PT_PIXELS },
};
static const sl_pdef_t canvas_defs[] = {
    { "w", SLATE_PROP_W, PT_CSIZE },
    { "h", SLATE_PROP_H, PT_CSIZE },
};
static const sl_pdef_t image_defs[] = {
    { "src", SLATE_PROP_IMAGE, PT_SRC },
};

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

/* index SL_BASE_COUNT: a user class that extends Element directly (no properties of Slate, an empty group) */
static sl_prop_t s_props[SL_BASE_COUNT + 1][PROPS_MAX];
static int s_nprops[SL_BASE_COUNT + 1];
static zend_class_entry *s_base_ce[SL_BASE_COUNT];
static const uint8_t s_kind[SL_BASE_COUNT + 1] = {
    [SL_GROUP] = SLATE_EL_GROUP, [SL_ROUNDRECT] = SLATE_EL_ROUNDRECT, [SL_CIRCLE] = SLATE_EL_CIRCLE,
    [SL_ARC] = SLATE_EL_ARC, [SL_LINE] = SLATE_EL_LINE, [SL_TEXT] = SLATE_EL_TEXT, [SL_CANVAS] = SLATE_EL_CANVAS,
    [SL_IMAGE] = SLATE_EL_IMAGE, [SL_SCENE] = SLATE_EL_GROUP, [SL_BASE_COUNT] = SLATE_EL_GROUP,
};

static zend_string *s_gradient_name, *s_shadow_name, *s_caps_name, *s_align_name;

const char *sl_short_name(zend_class_entry *ce)
{
    const char *n = ZSTR_VAL(ce->name), *s = strrchr(n, '\\');
    return s ? s + 1 : n;
}

static const sl_prop_t *find_prop(const sl_obj_t *o, zend_string *name)
{
    const sl_prop_t *p = s_props[o->base];
    for (int i = 0; i < s_nprops[o->base]; i++) {
        if (p[i].name == name || zend_string_equals(p[i].name, name)) {
            return &p[i];
        }
    }
    return NULL;
}

static const sl_prop_t *find_prop_c(const sl_obj_t *o, const char *name)
{
    const sl_prop_t *p = s_props[o->base];
    for (int i = 0; i < s_nprops[o->base]; i++) {
        if (strcmp(p[i].def->name, name) == 0) {
            return &p[i];
        }
    }
    return NULL;
}

static zval *prop_slot(sl_obj_t *o, const sl_prop_t *p)
{
    return OBJ_PROP(&o->std, p->offset);
}

/* ---- values ---- */

static const struct display_font_entry *font_find(const char *name, size_t len)
{
    for (size_t i = 0; i < display_fonts_count; i++) {
        const struct display_font_entry *e = &display_fonts[i];
        if (e->name && (e->gfx || e->ext) && strlen(e->name) == len && memcmp(e->name, name, len) == 0) {
            return e;
        }
    }
    return NULL;
}

static void bad_value(sl_obj_t *o, const sl_prop_t *p, const char *what)
{
    zend_value_error("%s::$%s %s", ZSTR_VAL(o->std.ce->name), p->def->name, what);
}

static void bad_font(sl_obj_t *o, zend_string *name)
{
    smart_str list = {0};
    for (size_t i = 0; i < display_fonts_count; i++) {
        if (display_fonts[i].name && (display_fonts[i].gfx || display_fonts[i].ext)) {
            if (list.s) {
                smart_str_appends(&list, ", ");
            }
            smart_str_appends(&list, display_fonts[i].name);
        }
    }
    smart_str_0(&list);
    zend_value_error("%s::$font must be a font of the firmware: unknown font '%s' (available: %s)",
                     ZSTR_VAL(o->std.ce->name), ZSTR_VAL(name), list.s ? ZSTR_VAL(list.s) : "none");
    smart_str_free(&list);
}

static bool num_in(const zval *v, double lo, double hi, double *out)
{
    double d = Z_TYPE_P(v) == IS_LONG ? (double)Z_LVAL_P(v) : Z_TYPE_P(v) == IS_DOUBLE ? Z_DVAL_P(v) : NAN;
    *out = d;
    return isfinite(d) && d >= lo && d <= hi;
}

static int enum_index(const zval *v, const char *const *names, int n)
{
    if (Z_TYPE_P(v) != IS_OBJECT || !(Z_OBJCE_P(v)->ce_flags & ZEND_ACC_ENUM)) {
        return 0;
    }
    zval *bv = zend_enum_fetch_case_value(Z_OBJ_P(v));
    for (int i = 0; i < n; i++) {
        if (Z_TYPE_P(bv) == IS_STRING && strcmp(Z_STRVAL_P(bv), names[i]) == 0) {
            return i;
        }
    }
    return 0;
}

static const char *const caps_names[] = { "butt", "round" };              /* slate_caps_t order */
static const char *const align_names[] = { "left", "center", "right" };   /* slate_align_t order */

/* The value v of property p as a command value (grad: storage for a gradient paint). check: validate and throw
   \ValueError (false) when it is out of range; without check (realize) the value was validated when assigned. */
static bool to_value(sl_obj_t *o, const sl_prop_t *p, zval *v, slate_value_t *out, slate_paint_grad_t *grad,
                     bool check)
{
    double d;
    uint32_t argb;
    ZVAL_DEREF(v);   /* a slot is never a reference (sl_read_property), but never trust it */
    switch ((sl_ptype_t)p->def->type) {
    case PT_COORD:
        if (!num_in(v, -32767, 32767, &d) && check) {
            bad_value(o, p, "must be a finite number between -32767 and 32767");
            return false;
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_SIZE:
        if (!num_in(v, 0, 32767, &d) && check) {
            bad_value(o, p, "must be between 0 and 32767");
            return false;
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_ANGLE:
        if (!num_in(v, -1e6, 1e6, &d) && check) {
            bad_value(o, p, "must be a finite number of degrees, between -1000000 and 1000000");
            return false;
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_OPACITY:
        if (!num_in(v, 0, 1, &d) && check) {
            bad_value(o, p, "must be between 0 and 1");
            return false;
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_BOOL:
        *out = slate_v_bool(Z_TYPE_P(v) == IS_TRUE);
        return true;
    case PT_NCOLOR:
        if (Z_TYPE_P(v) == IS_NULL) {
            *out = slate_v_color(0);
            return true;
        }
        ZEND_FALLTHROUGH;
    case PT_COLOR:
        if (!sl_color_of(v, &argb)) {
            if (check) {
                bad_value(o, p, "must be a color, 0xRRGGBB or 0xAARRGGBB");
                return false;
            }
            argb = 0;
        }
        *out = slate_v_color(argb);
        return true;
    case PT_NPAINT:
    case PT_PAINT:
        if (Z_TYPE_P(v) == IS_NULL) {
            *out = slate_v_paint((slate_paint_t){ 0, NULL });
            return true;
        }
        if (Z_TYPE_P(v) == IS_OBJECT && instanceof_function(Z_OBJCE_P(v), sl_gradient_ce)) {
            slate_paint_t paint;
            if (!sl_gradient_paint(Z_OBJ_P(v), &paint, grad)) {
                if (check) {
                    bad_value(o, p, "must be a LinearGradient or a RadialGradient built with its constructor");
                    return false;
                }
                paint = (slate_paint_t){ 0, NULL };
            }
            *out = slate_v_paint(paint);
            return true;
        }
        if (!sl_color_of(v, &argb)) {
            if (check) {
                bad_value(o, p, "must be a color, 0xRRGGBB or 0xAARRGGBB, or a gradient");
                return false;
            }
            argb = 0;
        }
        *out = slate_v_paint((slate_paint_t){ argb, NULL });
        return true;
    case PT_TEXT:
        *out = slate_v_text(Z_TYPE_P(v) == IS_STRING ? Z_STRVAL_P(v) : "");
        return true;
    case PT_FONT: {
        const struct display_font_entry *f =
            Z_TYPE_P(v) == IS_STRING ? font_find(Z_STRVAL_P(v), Z_STRLEN_P(v)) : NULL;
        if (!f && check) {
            bad_font(o, Z_TYPE_P(v) == IS_STRING ? Z_STR_P(v) : ZSTR_EMPTY_ALLOC());
            return false;
        }
        *out = slate_v_font(f);
        return true;
    }
    case PT_CAPS:
        *out = slate_v_enum(enum_index(v, caps_names, 2));
        return true;
    case PT_ALIGN:
        *out = slate_v_enum(enum_index(v, align_names, 3));
        return true;
    case PT_SHADOW:
        *out = sl_shadow_value(Z_TYPE_P(v) == IS_OBJECT ? Z_OBJ_P(v) : NULL);
        return true;
    case PT_BLUR:
        if (!num_in(v, 0, 3 * SLATE_BLUR_RADIUS_MAX, &d)) {
            if (check) {
                bad_value(o, p, "must be between 0 and 765");
                return false;
            }
            d = 0;
        }
        if (d > 0 && !SLATE_HAS_BLUR) {
            if (check) {
                sl_not_built("blur");
                return false;
            }
            d = 0;
        }
        *out = slate_v_num((float)sl_blur_radius((zend_long)d));
        return true;
    case PT_PIXELS:
        if (!num_in(v, 0, 32767, &d) && check) {
            bad_value(o, p, "must be between 0 and 32767");
            return false;
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_CSIZE:
        if (!num_in(v, 1, 1024, &d) && check) {
            bad_value(o, p, "must be between 1 and 1024");
            return false;
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_SRC:
        if (check && !sl_image_src_ok(v)) {
            return false;
        }
        *out = slate_v_image(o->im);   /* sl_assign binds the new path's image after the checks */
        return true;
    }
    return false;
}

static double slot_num(sl_obj_t *o, const char *name)
{
    const sl_prop_t *p = find_prop_c(o, name);
    return p ? zval_get_double(prop_slot(o, p)) : 0;
}

static bool slot_true(sl_obj_t *o, const char *name)
{
    const sl_prop_t *p = find_prop_c(o, name);
    return p && Z_TYPE_P(prop_slot(o, p)) == IS_TRUE;
}

/* Rules between properties, after p changed: a clipping group needs a clip box. */
static bool cross_check(sl_obj_t *o, const sl_prop_t *p)
{
    if (o->base == SL_GROUP && (p->def->prop == SLATE_PROP_CLIP || p->def->prop == SLATE_PROP_W
                                || p->def->prop == SLATE_PROP_H)) {
        if (slot_true(o, "clip") && (slot_num(o, "w") <= 0 || slot_num(o, "h") <= 0)) {
            zend_value_error("%s::$clip needs $w and $h greater than 0 (the clip box)", ZSTR_VAL(o->std.ce->name));
            return false;
        }
    }
    return true;
}

static bool realized(const sl_obj_t *o)
{
    return sl_g.engine && o->gen == sl_g.gen;
}

static bool push_set(sl_obj_t *o, const sl_prop_t *p, const slate_value_t *v)
{
    slate_cmd_t c = { .kind = SLATE_CMD_SET, .id = o->id, .prop = p->def->prop, .v = *v };
    return sl_push(&c);
}

/* A write of a Slate property: PHP's typed write, then validation (the old value back on a refusal), then the
   SET of a realized element. */
static zend_result sl_assign(sl_obj_t *o, const sl_prop_t *p, zval *value)
{
    zval *slot = prop_slot(o, p);
    zval old;
    ZVAL_COPY(&old, slot);
    zend_std_write_property(&o->std, p->name, value, NULL);
    if (EG(exception)) {
        zval_ptr_dtor(&old);
        return FAILURE;
    }
    slate_value_t v;
    slate_paint_grad_t grad;
    if (!to_value(o, p, slot, &v, &grad, true) || !cross_check(o, p)) {
        zval tmp;
        ZVAL_COPY_VALUE(&tmp, slot);
        ZVAL_COPY_VALUE(slot, &old);
        zval_ptr_dtor(&tmp);
        return FAILURE;
    }
    /* The same value again (the same number, string or enum case, or the same gradient or shadow object, which are
       immutable) changes nothing: no SET, so nothing is redrawn and the compositor is not woken. */
    bool same = Z_TYPE(old) != IS_ARRAY && zend_is_identical(&old, slot);
    /* An image's path: the image of the new path (or of the same one again, when it failed: a file written or a
       card mounted since). */
    if (p->def->type == PT_SRC && (!same || sl_image_failed(o))) {
        if (!sl_image_bind(o, Z_STR_P(slot))) {
            zval tmp;
            ZVAL_COPY_VALUE(&tmp, slot);
            ZVAL_COPY_VALUE(slot, &old);
            zval_ptr_dtor(&tmp);
            return FAILURE;
        }
        v = slate_v_image(o->im);
        same = false;
    }
    zval_ptr_dtor(&old);
    /* A canvas resized: the PHP buffer is cleared here, and the engine clears its own copy when the SET of W or H
       arrives (scene.h), so no pixels need to be sent again. */
    if (o->base == SL_CANVAS && (p->def->prop == SLATE_PROP_W || p->def->prop == SLATE_PROP_H)) {
        if (!sl_canvas_resize(o, (int)slot_num(o, "w"), (int)slot_num(o, "h"))) {
            return FAILURE;
        }
    }
    if (!same && realized(o) && !push_set(o, p, &v)) {
        return FAILURE;
    }
    return SUCCESS;
}

/* ---- handlers ---- */

static zval *sl_write_property(zend_object *obj, zend_string *name, zval *value, void **cache_slot)
{
    const sl_prop_t *p = find_prop(sl_obj(obj), name);
    if (!p) {
        return zend_std_write_property(obj, name, value, cache_slot);
    }
    if (sl_assign(sl_obj(obj), p, value) != SUCCESS) {
        return &EG(error_zval);
    }
    return OBJ_PROP(obj, p->offset);
}

/* A Slate property cannot be fetched for writing (a reference, a by-reference argument, $el->x[] = ...): the
   reference would change it without validation and without reaching the compositor. Reads are plain. */
static zval *sl_read_property(zend_object *obj, zend_string *name, int type, void **cache_slot, zval *rv)
{
    if (type != BP_VAR_R && type != BP_VAR_IS && find_prop(sl_obj(obj), name)) {
        zend_throw_error(NULL, "Cannot take a reference to %s::$%s", ZSTR_VAL(obj->ce->name), ZSTR_VAL(name));
        return &EG(uninitialized_zval);
    }
    return zend_std_read_property(obj, name, type, cache_slot, rv);
}

static zval *sl_get_property_ptr_ptr(zend_object *obj, zend_string *name, int type, void **cache_slot)
{
    if (find_prop(sl_obj(obj), name)) {
        return NULL;   /* compound assignments and ++ go through read + write */
    }
    return zend_std_get_property_ptr_ptr(obj, name, type, cache_slot);
}

static void sl_unset_property(zend_object *obj, zend_string *name, void **cache_slot)
{
    if (find_prop(sl_obj(obj), name)) {
        zend_throw_error(NULL, "Cannot unset %s::$%s", ZSTR_VAL(obj->ce->name), ZSTR_VAL(name));
        return;
    }
    zend_std_unset_property(obj, name, cache_slot);
}

#if PHP_VERSION_ID >= 80400
/* foreach over an element: by value as for any object (the iterator of PHP 8.4, which reads the declared properties
   with their visibility, then the dynamic ones); by reference refused. */
static zend_object_iterator *sl_get_iterator(zend_class_entry *ce, zval *object, int by_ref)
{
    if (by_ref) {
        zend_throw_error(NULL, "Cannot iterate %s by reference: assign its properties one by one",
                         ZSTR_VAL(Z_OBJCE_P(object)->name));
        return NULL;
    }
    return zend_hooked_object_get_iterator(ce, object, by_ref);
}
#endif

static HashTable *sl_get_gc(zend_object *obj, zval **table, int *n)
{
    sl_obj_t *o = sl_obj(obj);
    zend_get_gc_buffer *buf = zend_get_gc_buffer_create();
    /* As zend_std_get_gc: the declared properties either through the slots or, once the properties hash exists
       (get_object_vars(), var_dump(), foreach...), through it (it holds INDIRECT entries to the same slots), never
       both: an edge counted twice makes the GC free an object that is still referenced. */
    if (!obj->properties) {
        for (int i = 0; i < obj->ce->default_properties_count; i++) {
            zend_get_gc_buffer_add_zval(buf, &obj->properties_table[i]);
        }
    }
    if (o->has_children) {
        zval *zv;
        ZEND_HASH_FOREACH_VAL(&o->children, zv) {
            zend_get_gc_buffer_add_zval(buf, zv);
        } ZEND_HASH_FOREACH_END();
    }
    zend_get_gc_buffer_use(buf, table, n);
    return obj->properties;
}

static void sl_free_obj(zend_object *obj)
{
    sl_obj_t *o = sl_obj(obj);
    if (realized(o)) {
        /* no exception from here: a refusal only leaves the element in the engine until it stops */
        slate_cmd_t c = { .kind = SLATE_CMD_FREE, .id = o->id };
        slate_cmd_push(slate_engine_cmds(sl_g.engine), &c);
    }
    if (o->has_children) {
        zval *zv;
        ZEND_HASH_FOREACH_VAL(&o->children, zv) {
            if (Z_TYPE_P(zv) == IS_OBJECT) {
                sl_obj(Z_OBJ_P(zv))->parent = NULL;
            }
        } ZEND_HASH_FOREACH_END();
        zend_hash_destroy(&o->children);
        o->has_children = false;
    }
    if (o->px) {
        efree(o->px);
        o->px = NULL;
    }
    sl_image_unbind(o);
    zend_object_std_dtor(obj);
}

static int base_of(zend_class_entry *ce)
{
    for (zend_class_entry *c = ce; c; c = c->parent) {
        for (int b = 0; b < SL_BASE_COUNT; b++) {
            if (s_base_ce[b] == c) {
                return b;
            }
        }
    }
    return SL_BASE_COUNT;
}

static void default_of(int base, const sl_pdef_t *d, zval *out)
{
    switch ((sl_ptype_t)d->type) {
    case PT_BOOL:
        ZVAL_BOOL(out, d->prop == SLATE_PROP_VISIBLE);
        return;
    case PT_OPACITY:
        ZVAL_DOUBLE(out, 1.0);
        return;
    case PT_COLOR:
    case PT_PAINT:
        ZVAL_LONG(out, 0xFFFFFF);
        return;
    case PT_NCOLOR:
    case PT_NPAINT:
    case PT_SHADOW:
        ZVAL_NULL(out);
        return;
    case PT_TEXT:
    case PT_SRC:
        ZVAL_EMPTY_STRING(out);
        return;
    case PT_FONT:
        ZVAL_STRINGL(out, "default", 7);
        return;
    case PT_CAPS:
        ZVAL_OBJ_COPY(out, zend_enum_get_case_cstr(sl_caps_ce, base == SL_ARC ? "Round" : "Butt"));
        return;
    case PT_ALIGN:
        ZVAL_OBJ_COPY(out, zend_enum_get_case_cstr(sl_align_ce, "Left"));
        return;
    default:
        ZVAL_LONG(out, d->prop == SLATE_PROP_WIDTH && base == SL_LINE ? 1 : 0);
        return;
    }
}

static void not_an_element(zend_class_entry *ce)
{
    zend_throw_exception_ex(spl_ce_LogicException, 0,
                            "%s: extend Baremetal\\Slate\\Group to build a custom element", ZSTR_VAL(ce->name));
}

/* new of a user class that extends Element directly (not Group nor a built-in element): refused, it would be an
   element without properties of Slate. */
static zend_function *sl_get_constructor(zend_object *obj)
{
    if (sl_obj(obj)->base == SL_BASE_COUNT) {
        not_an_element(obj->ce);
        return NULL;
    }
    return zend_std_get_constructor(obj);
}

zend_object *sl_obj_create(zend_class_entry *ce)
{
    sl_obj_t *o = zend_object_alloc(sizeof(sl_obj_t), ce);
    memset(o, 0, XtOffsetOf(sl_obj_t, std));
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &sl_handlers;
    o->base = (uint8_t)base_of(ce);
    o->kind = s_kind[o->base];
    if (++s_next_id == 0 || s_next_id == UINT32_MAX) {
        s_next_id = 1;
    }
    o->id = s_next_id;
    o->dx0 = o->dy0 = 0;
    o->dx1 = o->dy1 = 0;
    if (o->base == SL_GROUP || o->base == SL_SCENE) {
        zend_hash_init(&o->children, 4, NULL, ZVAL_PTR_DTOR, 0);
        o->has_children = true;
    }
    for (int i = 0; i < s_nprops[o->base]; i++) {
        const sl_prop_t *p = &s_props[o->base][i];
        zval *slot = prop_slot(o, p);
        zval_ptr_dtor(slot);
        default_of(o->base, p->def, slot);
    }
    if (o->base == SL_IMAGE) {
        sl_image_init(o);
    }
    return &o->std;
}

/* ---- realization ---- */

bool sl_realize(sl_obj_t *o)
{
    if (!sl_g.engine || o->gen == sl_g.gen) {
        return true;
    }
    slate_cmd_t c = { .kind = SLATE_CMD_NEW, .id = o->id, .el = (slate_el_kind_t)o->kind };
    if (!sl_push(&c)) {
        return false;
    }
    o->gen = sl_g.gen;
    for (int i = 0; i < s_nprops[o->base]; i++) {
        const sl_prop_t *p = &s_props[o->base][i];
        slate_value_t v;
        slate_paint_grad_t grad;
        to_value(o, p, prop_slot(o, p), &v, &grad, false);
        if (!push_set(o, p, &v)) {
            return false;
        }
    }
    if (o->base == SL_CANVAS && o->px && !sl_canvas_send(o, 0, 0, o->cw, o->ch)) {
        return false;
    }
    if (o->has_children) {
        zval *zv;
        ZEND_HASH_FOREACH_VAL(&o->children, zv) {
            sl_obj_t *ch = sl_obj(Z_OBJ_P(zv));
            if (!sl_realize(ch)) {
                return false;
            }
            slate_cmd_t add = { .kind = SLATE_CMD_ADD, .id = ch->id, .parent = o->id };
            if (!sl_push(&add)) {
                return false;
            }
        } ZEND_HASH_FOREACH_END();
    }
    return true;
}

/* ---- children ---- */

void sl_children_add(sl_obj_t *o, zval *args, uint32_t argc)
{
    for (uint32_t i = 0; i < argc; i++) {
        zval *zv = &args[i];
        sl_obj_t *c = sl_obj(Z_OBJ_P(zv));
        if (c->base == SL_BASE_COUNT) {
            not_an_element(c->std.ce);   /* made without its constructor (reflection) */
            return;
        }
        if (c->parent) {
            zend_throw_exception_ex(spl_ce_LogicException, 0,
                                    "%s already belongs to a group: remove() it from there first",
                                    ZSTR_VAL(c->std.ce->name));
            return;
        }
        for (sl_obj_t *a = o; a; a = a->parent) {
            if (a == c) {
                zend_throw_exception_ex(spl_ce_LogicException, 0, "%s cannot contain itself",
                                        ZSTR_VAL(c->std.ce->name));
                return;
            }
        }
        Z_ADDREF_P(zv);
        zend_hash_next_index_insert(&o->children, zv);
        c->parent = o;
        if (realized(o)) {
            if (!sl_realize(c)) {
                return;
            }
            slate_cmd_t add = { .kind = SLATE_CMD_ADD, .id = c->id, .parent = o->id };
            if (!sl_push(&add)) {
                return;
            }
        }
    }
}

void sl_children_remove(sl_obj_t *o, zval *args, uint32_t argc)
{
    for (uint32_t i = 0; i < argc; i++) {
        sl_obj_t *c = sl_obj(Z_OBJ(args[i]));
        if (c->parent != o) {
            continue;   /* not a child of this one: nothing to do */
        }
        zend_ulong idx;
        zval *zv;
        bool found = false;
        ZEND_HASH_FOREACH_NUM_KEY_VAL(&o->children, idx, zv) {
            if (Z_OBJ_P(zv) == &c->std) {
                found = true;
                break;
            }
        } ZEND_HASH_FOREACH_END();
        if (!found) {
            continue;
        }
        bool send = realized(o) && realized(c);
        c->parent = NULL;
        if (send) {
            slate_cmd_t rm = { .kind = SLATE_CMD_REMOVE, .id = c->id };
            if (!sl_push(&rm)) {
                zend_hash_index_del(&o->children, idx);
                return;
            }
        }
        zend_hash_index_del(&o->children, idx);   /* may free c (FREE after REMOVE) */
    }
}

/* ---- constructors ---- */

/* Assigns the arguments in order to the properties names[i] (NULL: skip). */
static void construct(INTERNAL_FUNCTION_PARAMETERS, const char *const *names, uint32_t min, uint32_t max)
{
    zval *args = NULL;
    uint32_t argc = 0;
    /* -1: a fixed maximum with Z_PARAM_VARIADIC breaks the parser's assumption (_i == max) in release builds */
    ZEND_PARSE_PARAMETERS_START(min, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    if (argc > max) {
        zend_wrong_parameters_count_error(min, max);
        RETURN_THROWS();
    }
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_ISUNDEF(args[i]) || !names[i]) {
            continue;
        }
        const sl_prop_t *p = find_prop_c(o, names[i]);
        if (p && sl_assign(o, p, &args[i]) != SUCCESS) {
            RETURN_THROWS();
        }
    }
}

static const char *const roundrect_args[] = { "x", "y", "w", "h", "radius", "fill", "border", "borderColor", "shadow" };
static const char *const circle_args[] = { "x", "y", "r", "fill", "border", "borderColor", "shadow" };
static const char *const arc_args[] = { "x", "y", "r", "from", "to", "width", "color", "caps" };
static const char *const line_args[] = { "x", "y", "x1", "y1", "width", "color", "caps" };
static const char *const text_args[] = { "x", "y", "text", "font", "color", "align", "maxWidth" };
static const char *const group_args[] = { "x", "y", "w", "h", "opacity", "clip", "cache", "blur", "shadow" };
static const char *const canvas_args[] = { "x", "y", "w", "h" };
static const char *const image_args[] = { "x", "y", "src", "opacity" };

PHP_METHOD(SlateRoundRect, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, roundrect_args, 4, 9);
}

PHP_METHOD(SlateCircle, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, circle_args, 3, 7);
}

PHP_METHOD(SlateArc, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, arc_args, 7, 8);
}

PHP_METHOD(SlateLine, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, line_args, 4, 7);
}

PHP_METHOD(SlateText, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, text_args, 3, 7);
}

PHP_METHOD(SlateGroup, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, group_args, 0, 9);
}

PHP_METHOD(SlateCanvas, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, canvas_args, 4, 4);
}

PHP_METHOD(SlateImage, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, image_args, 3, 4);
}

/* ---- Element::set(), Group::add(), Group::remove() ---- */

PHP_METHOD(SlateElement, set)
{
    zval *args = NULL;
    uint32_t argc = 0;
    HashTable *named = NULL;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC_WITH_NAMED(args, argc, named)
    ZEND_PARSE_PARAMETERS_END();
    (void)args;
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    if (argc > 0) {
        zend_argument_count_error("%s::set() takes named arguments only, e.g. set(x: 10, opacity: 0.5)",
                                  ZSTR_VAL(obj->ce->name));
        RETURN_THROWS();
    }
    if (named) {
        zend_string *key;
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(named, key, val) {
            zend_property_info *info = zend_hash_find_ptr(&obj->ce->properties_info, key);
            if (!info || !(info->flags & ZEND_ACC_PUBLIC) || (info->flags & ZEND_ACC_STATIC)) {
                zend_throw_error(NULL, "%s::set(): unknown property $%s", ZSTR_VAL(obj->ce->name), ZSTR_VAL(key));
                RETURN_THROWS();
            }
            obj->handlers->write_property(obj, key, val, NULL);
            if (EG(exception)) {
                RETURN_THROWS();
            }
        } ZEND_HASH_FOREACH_END();
    }
    RETURN_OBJ_COPY(obj);
}

PHP_METHOD(SlateGroup, add)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_OBJECT || !instanceof_function(Z_OBJCE(args[i]), sl_element_ce)) {
            zend_argument_type_error(i + 1, "must be of type Baremetal\\Slate\\Element, %s given",
                                     zend_zval_value_name(&args[i]));
            RETURN_THROWS();
        }
    }
    sl_children_add(sl_obj(Z_OBJ_P(ZEND_THIS)), args, argc);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

PHP_METHOD(SlateGroup, remove)
{
    zval *args = NULL;
    uint32_t argc = 0;
    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', args, argc)
    ZEND_PARSE_PARAMETERS_END();
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_OBJECT || !instanceof_function(Z_OBJCE(args[i]), sl_element_ce)) {
            zend_argument_type_error(i + 1, "must be of type Baremetal\\Slate\\Element, %s given",
                                     zend_zval_value_name(&args[i]));
            RETURN_THROWS();
        }
    }
    sl_children_remove(sl_obj(Z_OBJ_P(ZEND_THIS)), args, argc);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* ---- arginfo ---- */

#define T_NUM (MAY_BE_LONG | MAY_BE_DOUBLE)

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_element_set, 0, 0, IS_STATIC, 0)
    ZEND_ARG_VARIADIC_TYPE_INFO(0, props, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_group_children, 0, 0, IS_STATIC, 0)
    ZEND_ARG_VARIADIC_OBJ_INFO(0, e, Baremetal\\Slate\\Element, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_roundrect_ctor, 0, 0, 4)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, w, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, h, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, radius, T_NUM, "0")
    ZEND_ARG_OBJ_TYPE_MASK(0, fill, Baremetal\\Slate\\Gradient, T_NUM | MAY_BE_NULL, "null")
    ZEND_ARG_TYPE_MASK(0, border, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, borderColor, T_NUM | MAY_BE_NULL, "null")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, shadow, Baremetal\\Slate\\Shadow, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_circle_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, cx, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, cy, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, r, T_NUM, NULL)
    ZEND_ARG_OBJ_TYPE_MASK(0, fill, Baremetal\\Slate\\Gradient, T_NUM | MAY_BE_NULL, "null")
    ZEND_ARG_TYPE_MASK(0, border, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, borderColor, T_NUM | MAY_BE_NULL, "null")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, shadow, Baremetal\\Slate\\Shadow, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_arc_ctor, 0, 0, 7)
    ZEND_ARG_TYPE_MASK(0, cx, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, cy, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, r, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, from, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, to, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, width, T_NUM, NULL)
    ZEND_ARG_OBJ_TYPE_MASK(0, color, Baremetal\\Slate\\Gradient, T_NUM, NULL)
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, caps, Baremetal\\Slate\\Caps, 0, "\\Baremetal\\Slate\\Caps::Round")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_line_ctor, 0, 0, 4)
    ZEND_ARG_TYPE_MASK(0, x0, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, y0, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, x1, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, y1, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, width, T_NUM, "1")
    ZEND_ARG_TYPE_MASK(0, color, T_NUM, "0xFFFFFF")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, caps, Baremetal\\Slate\\Caps, 0, "\\Baremetal\\Slate\\Caps::Butt")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_text_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, NULL)
    ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, font, IS_STRING, 0, "'default'")
    ZEND_ARG_TYPE_MASK(0, color, T_NUM, "0xFFFFFF")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, align, Baremetal\\Slate\\Align, 0, "\\Baremetal\\Slate\\Align::Left")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxWidth, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_group_ctor, 0, 0, 0)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, w, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, h, T_NUM, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, opacity, IS_DOUBLE, 0, "1.0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, clip, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cache, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, blur, IS_LONG, 0, "0")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, shadow, Baremetal\\Slate\\Shadow, 1, "null")
ZEND_END_ARG_INFO()


static const zend_function_entry element_methods[] = {
    PHP_ME(SlateElement, set, arginfo_element_set, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry group_methods[] = {
    PHP_ME(SlateGroup, __construct, arginfo_group_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateGroup, add, arginfo_group_children, ZEND_ACC_PUBLIC)
    PHP_ME(SlateGroup, remove, arginfo_group_children, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry roundrect_methods[] = {
    PHP_ME(SlateRoundRect, __construct, arginfo_roundrect_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry circle_methods[] = {
    PHP_ME(SlateCircle, __construct, arginfo_circle_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry arc_methods[] = {
    PHP_ME(SlateArc, __construct, arginfo_arc_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry line_methods[] = {
    PHP_ME(SlateLine, __construct, arginfo_line_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
static const zend_function_entry text_methods[] = {
    PHP_ME(SlateText, __construct, arginfo_text_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ---- registration ---- */

static zend_type type_of(uint8_t t)
{
    switch ((sl_ptype_t)t) {
    case PT_OPACITY:
        return (zend_type)ZEND_TYPE_INIT_CODE(IS_DOUBLE, 0, 0);
    case PT_BOOL:
        return (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0);
    case PT_NCOLOR:
        return (zend_type)ZEND_TYPE_INIT_MASK(T_NUM | MAY_BE_NULL);
    case PT_PAINT:
        return (zend_type)ZEND_TYPE_INIT_CLASS(s_gradient_name, 0, T_NUM);
    case PT_NPAINT:
        return (zend_type)ZEND_TYPE_INIT_CLASS(s_gradient_name, 1, T_NUM);
    case PT_TEXT:
    case PT_FONT:
    case PT_SRC:
        return (zend_type)ZEND_TYPE_INIT_CODE(IS_STRING, 0, 0);
    case PT_CAPS:
        return (zend_type)ZEND_TYPE_INIT_CLASS(s_caps_name, 0, 0);
    case PT_ALIGN:
        return (zend_type)ZEND_TYPE_INIT_CLASS(s_align_name, 0, 0);
    case PT_SHADOW:
        return (zend_type)ZEND_TYPE_INIT_CLASS(s_shadow_name, 1, 0);
    case PT_BLUR:
    case PT_PIXELS:
    case PT_CSIZE:
        return (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0);
    default:   /* COORD, SIZE, ANGLE, COLOR */
        return (zend_type)ZEND_TYPE_INIT_MASK(T_NUM);
    }
}

static void declare(zend_class_entry *ce, const sl_pdef_t *defs, int n)
{
    for (int i = 0; i < n; i++) {
        zval undef;
        ZVAL_UNDEF(&undef);
        zend_declare_typed_property(ce, zend_string_init_interned(defs[i].name, strlen(defs[i].name), 1), &undef,
                                    ZEND_ACC_PUBLIC, NULL, type_of(defs[i].type));
    }
}

/* The property table of a base class: the common properties, then its own, with their slots. */
static void table(sl_base_t b, const sl_pdef_t *defs, int n)
{
    zend_class_entry *ce = s_base_ce[b];
    int k = 0;
    for (int pass = 0; pass < 2; pass++) {
        const sl_pdef_t *d = pass == 0 ? common_defs : defs;
        int m = pass == 0 ? (b == SL_SCENE ? 0 : N(common_defs)) : n;
        for (int i = 0; i < m && k < PROPS_MAX; i++) {
            zend_property_info *info = zend_hash_str_find_ptr(&ce->properties_info, d[i].name, strlen(d[i].name));
            if (!info) {
                continue;
            }
            s_props[b][k].def = &d[i];
            s_props[b][k].name = zend_string_init_interned(d[i].name, strlen(d[i].name), 1);
            s_props[b][k].offset = info->offset;
            k++;
        }
    }
    s_nprops[b] = k;
}

static zend_class_entry *leaf(const char *name, const zend_function_entry *methods, const sl_pdef_t *defs, int n,
                              bool final)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), methods);
    zend_class_entry *c = zend_register_internal_class_ex(&ce, sl_element_ce);
    c->ce_flags |= (final ? ZEND_ACC_FINAL : 0) | ZEND_ACC_NOT_SERIALIZABLE;
    declare(c, defs, n);
    return c;
}

void sl_elements_register(void)
{
    s_gradient_name = zend_string_init_interned("Baremetal\\Slate\\Gradient", sizeof("Baremetal\\Slate\\Gradient") - 1, 1);
    s_shadow_name = zend_string_init_interned("Baremetal\\Slate\\Shadow", sizeof("Baremetal\\Slate\\Shadow") - 1, 1);
    s_caps_name = zend_string_init_interned("Baremetal\\Slate\\Caps", sizeof("Baremetal\\Slate\\Caps") - 1, 1);
    s_align_name = zend_string_init_interned("Baremetal\\Slate\\Align", sizeof("Baremetal\\Slate\\Align") - 1, 1);

    memcpy(&sl_handlers, zend_get_std_object_handlers(), sizeof sl_handlers);
    sl_handlers.offset = XtOffsetOf(sl_obj_t, std);
    sl_handlers.free_obj = sl_free_obj;
    sl_handlers.clone_obj = NULL;
    sl_handlers.write_property = sl_write_property;
    sl_handlers.get_property_ptr_ptr = sl_get_property_ptr_ptr;
    sl_handlers.read_property = sl_read_property;
    sl_handlers.get_constructor = sl_get_constructor;
    sl_handlers.unset_property = sl_unset_property;
    sl_handlers.get_gc = sl_get_gc;

    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Element", element_methods);
    sl_element_ce = zend_register_internal_class(&ce);
    sl_element_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | ZEND_ACC_NOT_SERIALIZABLE;
    sl_element_ce->create_object = sl_obj_create;
#if PHP_VERSION_ID >= 80400
    sl_element_ce->get_iterator = sl_get_iterator;   /* inherited by every element class */
#endif
    declare(sl_element_ce, common_defs, N(common_defs));

    sl_group_ce = leaf("Baremetal\\Slate\\Group", group_methods, group_defs, N(group_defs), false);
    sl_roundrect_ce = leaf("Baremetal\\Slate\\RoundRect", roundrect_methods, roundrect_defs, N(roundrect_defs), true);
    sl_circle_ce = leaf("Baremetal\\Slate\\Circle", circle_methods, circle_defs, N(circle_defs), true);
    sl_arc_ce = leaf("Baremetal\\Slate\\Arc", arc_methods, arc_defs, N(arc_defs), true);
    sl_line_ce = leaf("Baremetal\\Slate\\Line", line_methods, line_defs, N(line_defs), true);
    sl_text_ce = leaf("Baremetal\\Slate\\Text", text_methods, text_defs, N(text_defs), true);
    sl_canvas_ce = leaf("Baremetal\\Slate\\Canvas", sl_canvas_method_table(), canvas_defs, N(canvas_defs), true);
    sl_image_ce = leaf("Baremetal\\Slate\\Image", sl_image_method_table(), image_defs, N(image_defs), true);
    sl_image_class(sl_image_ce);

    s_base_ce[SL_GROUP] = sl_group_ce;
    s_base_ce[SL_ROUNDRECT] = sl_roundrect_ce;
    s_base_ce[SL_CIRCLE] = sl_circle_ce;
    s_base_ce[SL_ARC] = sl_arc_ce;
    s_base_ce[SL_LINE] = sl_line_ce;
    s_base_ce[SL_TEXT] = sl_text_ce;
    s_base_ce[SL_CANVAS] = sl_canvas_ce;
    s_base_ce[SL_IMAGE] = sl_image_ce;

    table(SL_GROUP, group_defs, N(group_defs));
    table(SL_ROUNDRECT, roundrect_defs, N(roundrect_defs));
    table(SL_CIRCLE, circle_defs, N(circle_defs));
    table(SL_ARC, arc_defs, N(arc_defs));
    table(SL_LINE, line_defs, N(line_defs));
    table(SL_TEXT, text_defs, N(text_defs));
    table(SL_CANVAS, canvas_defs, N(canvas_defs));
    table(SL_IMAGE, image_defs, N(image_defs));
}

void sl_elements_scene(zend_class_entry *scene_ce)
{
    s_base_ce[SL_SCENE] = scene_ce;
    scene_ce->create_object = sl_obj_create;
    table(SL_SCENE, NULL, 0);
}
