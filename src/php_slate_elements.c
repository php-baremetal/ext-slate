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
 * The elements: abstract Element (x, y, visible, opacity, set(), animate(), ease()), Group (add, remove), Scroll (a
 * Group; touch(), scrollTo(), its position and its events are in php_slate_scroll.c), RoundRect, Circle, Arc, Line,
 * Text, Canvas (its drawing methods are in php_slate_canvas.c) and Image (its image, wait() and the events are in
 * php_slate_images.c). See php_slate.h for the model; the Tween and its events are in
 * php_slate_anim.c.
 *
 * Properties are declared, typed public properties. The object handlers route a write of a Slate property through
 * sl_assign(): PHP checks the type (and coerces, as for any typed property), the value is validated here
 * (\ValueError "<Class>::$<name> must be ..." and the old value stays), and a realized element sends a SET.
 * get_property_ptr_ptr refuses those properties, so `$el->x += 4` and `$el->x++` go through the write handler
 * too; unset() of one throws, and so does foreach by reference over an element (its references would change the
 * properties without validation, as a reference taken one by one would).
 *
 * Touch ($touchable, $hitPad, $holdMs, declared on Element with their defaults: a subclass may give its own, as
 * `public bool $touchable = true;`). $touchable becomes a SET of TOUCHABLE that is true also while the element has a
 * callback of on(); $hitPad a SET of HIT_PAD; $holdMs stays in PHP (php_slate_touch.c reads it at a Press). None is
 * animatable. The methods onPress()..onCancel() are empty here, for subclasses to override; on() and off() keep the
 * callbacks (php_slate_touch.c).
 *
 * A Scroll's own properties do not all become SETs: $axis, $bounce, $snapX, $snapY, $snapMax and $indicators are its
 * set-up (a SCROLL CONFIG with all of them), $scrollX and $scrollY its position (a SCROLL JUMP: a SET would be
 * overwritten by the physics); reading those two gives the position drawn (php_slate_scroll.c), so their reads are
 * not cached.
 */
#include <math.h>
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "zend_closures.h"
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
    PT_VIEW,      /* int|float, a Scroll's view 1..32767 */
    PT_CLIPON,    /* bool, a Scroll's $clip: always true */
    PT_AXIS,      /* ScrollAxis */
    PT_SPOS,      /* int|float, a scroll position: finite (clamped by the engine) */
    PT_TOUCH,     /* bool, $touchable: TOUCHABLE is sent true also while the element has a callback */
    PT_HITPAD,    /* int, 0..SLATE_HIT_PAD_MAX */
    PT_HOLDMS,    /* int, 0..10000 (PHP only) */
} sl_ptype_t;

/* How a write reaches the engine (HOW_NONE: it does not, the PHP layer reads it). */
enum { HOW_SET = 0, HOW_CONFIG, HOW_JUMP, HOW_NONE };

typedef struct {
    const char *name;
    slate_prop_t prop;
    uint8_t type;   /* sl_ptype_t */
    uint8_t how;    /* HOW_*: a SET of prop, a Scroll's CONFIG (prop unused), a Scroll's JUMP */
} sl_pdef_t;

typedef struct {
    const sl_pdef_t *def;
    zend_string *name;
    uint32_t offset;
} sl_prop_t;

#define PROPS_MAX 28

/* The first COMMON_DRAWN are declared without a default (sl_obj_create gives each element its own); the touch ones
   after them with their defaults, which a subclass may redeclare (sl_obj_create keeps the class's). */
#define COMMON_DRAWN 4
#define HOLD_MS_MAX 10000
static const sl_pdef_t common_defs[] = {
    { "x", SLATE_PROP_X, PT_COORD, HOW_SET },
    { "y", SLATE_PROP_Y, PT_COORD, HOW_SET },
    { "visible", SLATE_PROP_VISIBLE, PT_BOOL, HOW_SET },
    { "opacity", SLATE_PROP_OPACITY, PT_OPACITY, HOW_SET },
    { "touchable", SLATE_PROP_TOUCHABLE, PT_TOUCH, HOW_SET },
    { "hitPad", SLATE_PROP_HIT_PAD, PT_HITPAD, HOW_SET },
    { "holdMs", SLATE_PROP_HIT_PAD, PT_HOLDMS, HOW_NONE },   /* prop unused */
};
static const sl_pdef_t group_defs[] = {
    { "w", SLATE_PROP_W, PT_SIZE, HOW_SET },
    { "h", SLATE_PROP_H, PT_SIZE, HOW_SET },
    { "clip", SLATE_PROP_CLIP, PT_BOOL, HOW_SET },
    { "cache", SLATE_PROP_CACHE, PT_BOOL, HOW_SET },
    { "blur", SLATE_PROP_BLUR, PT_BLUR, HOW_SET },
    { "shadow", SLATE_PROP_SHADOW, PT_SHADOW, HOW_SET },
};
/* Group's six first (the class declares only the others: it inherits those) */
#define SCROLL_INHERITED 6
static const sl_pdef_t scroll_defs[] = {
    { "w", SLATE_PROP_W, PT_VIEW, HOW_SET },
    { "h", SLATE_PROP_H, PT_VIEW, HOW_SET },
    { "clip", SLATE_PROP_CLIP, PT_CLIPON, HOW_SET },
    { "cache", SLATE_PROP_CACHE, PT_BOOL, HOW_SET },
    { "blur", SLATE_PROP_BLUR, PT_BLUR, HOW_SET },
    { "shadow", SLATE_PROP_SHADOW, PT_SHADOW, HOW_SET },
    { "contentW", SLATE_PROP_CONTENT_W, PT_PIXELS, HOW_SET },
    { "contentH", SLATE_PROP_CONTENT_H, PT_PIXELS, HOW_SET },
    { "axis", SLATE_PROP_SCROLL, PT_AXIS, HOW_CONFIG },
    { "bounce", SLATE_PROP_SCROLL, PT_BOOL, HOW_CONFIG },
    { "snapX", SLATE_PROP_SCROLL, PT_PIXELS, HOW_CONFIG },
    { "snapY", SLATE_PROP_SCROLL, PT_PIXELS, HOW_CONFIG },
    { "snapMax", SLATE_PROP_SCROLL, PT_PIXELS, HOW_CONFIG },
    { "indicators", SLATE_PROP_SCROLL, PT_BOOL, HOW_CONFIG },
    { "scrollX", SLATE_PROP_SCROLL_X, PT_SPOS, HOW_JUMP },
    { "scrollY", SLATE_PROP_SCROLL_Y, PT_SPOS, HOW_JUMP },
};
static const sl_pdef_t roundrect_defs[] = {
    { "w", SLATE_PROP_W, PT_SIZE, HOW_SET },
    { "h", SLATE_PROP_H, PT_SIZE, HOW_SET },
    { "radius", SLATE_PROP_RADIUS, PT_SIZE, HOW_SET },
    { "fill", SLATE_PROP_FILL, PT_NPAINT, HOW_SET },
    { "border", SLATE_PROP_BORDER, PT_SIZE, HOW_SET },
    { "borderColor", SLATE_PROP_BORDER_COLOR, PT_NCOLOR, HOW_SET },
    { "shadow", SLATE_PROP_SHADOW, PT_SHADOW, HOW_SET },
};
static const sl_pdef_t circle_defs[] = {
    { "r", SLATE_PROP_R, PT_SIZE, HOW_SET },
    { "fill", SLATE_PROP_FILL, PT_NPAINT, HOW_SET },
    { "border", SLATE_PROP_BORDER, PT_SIZE, HOW_SET },
    { "borderColor", SLATE_PROP_BORDER_COLOR, PT_NCOLOR, HOW_SET },
    { "shadow", SLATE_PROP_SHADOW, PT_SHADOW, HOW_SET },
};
static const sl_pdef_t arc_defs[] = {
    { "r", SLATE_PROP_R, PT_SIZE, HOW_SET },
    { "from", SLATE_PROP_FROM, PT_ANGLE, HOW_SET },
    { "to", SLATE_PROP_TO, PT_ANGLE, HOW_SET },
    { "width", SLATE_PROP_WIDTH, PT_SIZE, HOW_SET },
    { "color", SLATE_PROP_COLOR, PT_PAINT, HOW_SET },
    { "caps", SLATE_PROP_CAPS, PT_CAPS, HOW_SET },
};
static const sl_pdef_t line_defs[] = {
    { "x1", SLATE_PROP_X1, PT_COORD, HOW_SET },
    { "y1", SLATE_PROP_Y1, PT_COORD, HOW_SET },
    { "width", SLATE_PROP_WIDTH, PT_SIZE, HOW_SET },
    { "color", SLATE_PROP_COLOR, PT_COLOR, HOW_SET },
    { "caps", SLATE_PROP_CAPS, PT_CAPS, HOW_SET },
};
static const sl_pdef_t text_defs[] = {
    { "text", SLATE_PROP_TEXT, PT_TEXT, HOW_SET },
    { "font", SLATE_PROP_FONT, PT_FONT, HOW_SET },
    { "color", SLATE_PROP_COLOR, PT_COLOR, HOW_SET },
    { "align", SLATE_PROP_ALIGN, PT_ALIGN, HOW_SET },
    { "maxWidth", SLATE_PROP_MAX_WIDTH, PT_PIXELS, HOW_SET },
};
static const sl_pdef_t canvas_defs[] = {
    { "w", SLATE_PROP_W, PT_CSIZE, HOW_SET },
    { "h", SLATE_PROP_H, PT_CSIZE, HOW_SET },
};
static const sl_pdef_t image_defs[] = {
    { "src", SLATE_PROP_IMAGE, PT_SRC, HOW_SET },
};

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

/* index SL_BASE_COUNT: a user class that extends Element directly (no properties of Slate, an empty group) */
static sl_prop_t s_props[SL_BASE_COUNT + 1][PROPS_MAX];
static int s_nprops[SL_BASE_COUNT + 1];
static zend_class_entry *s_base_ce[SL_BASE_COUNT];
static const uint8_t s_kind[SL_BASE_COUNT + 1] = {
    [SL_GROUP] = SLATE_EL_GROUP, [SL_ROUNDRECT] = SLATE_EL_ROUNDRECT, [SL_CIRCLE] = SLATE_EL_CIRCLE,
    [SL_ARC] = SLATE_EL_ARC, [SL_LINE] = SLATE_EL_LINE, [SL_TEXT] = SLATE_EL_TEXT, [SL_CANVAS] = SLATE_EL_CANVAS,
    [SL_IMAGE] = SLATE_EL_IMAGE, [SL_SCROLL] = SLATE_EL_GROUP, [SL_SCENE] = SLATE_EL_GROUP,
    [SL_BASE_COUNT] = SLATE_EL_GROUP,
};

static zend_string *s_gradient_name, *s_shadow_name, *s_caps_name, *s_align_name, *s_axis_name;

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
    case PT_VIEW:
        if (!num_in(v, 1, 32767, &d) && check) {
            bad_value(o, p, "must be between 1 and 32767");
            return false;
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_CLIPON:
        if (Z_TYPE_P(v) != IS_TRUE && check) {
            bad_value(o, p, "is always true");
            return false;
        }
        *out = slate_v_bool(true);
        return true;
    case PT_AXIS:
        *out = slate_v_enum(sl_scroll_axis_of(v));   /* sent in a CONFIG, never as a SET */
        return true;
    case PT_SPOS:
        if (!num_in(v, -INFINITY, INFINITY, &d) && check) {
            bad_value(o, p, "must be a finite number");
            return false;
        }
        *out = slate_v_num(isfinite(d) ? (float)d : 0.0f);   /* sent in a JUMP, never as a SET */
        return true;
    case PT_TOUCH:
        *out = slate_v_bool(Z_TYPE_P(v) == IS_TRUE || sl_touch_has_cb(o));
        return true;
    case PT_HITPAD:
        if (!num_in(v, 0, SLATE_HIT_PAD_MAX, &d)) {
            if (check) {
                bad_value(o, p, "must be between 0 and 64");
                return false;
            }
            d = d > 0 ? SLATE_HIT_PAD_MAX : 0;   /* a subclass's default out of range */
        }
        *out = slate_v_num((float)d);
        return true;
    case PT_HOLDMS:
        if (!num_in(v, 0, HOLD_MS_MAX, &d) && check) {
            bad_value(o, p, "must be between 0 and 10000");
            return false;
        }
        *out = slate_v_num(0);   /* never sent */
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

zval *sl_slot(sl_obj_t *o, const char *name)
{
    const sl_prop_t *p = find_prop_c(o, name);
    return p ? prop_slot(o, p) : NULL;
}

double sl_slot_num(sl_obj_t *o, const char *name)
{
    return slot_num(o, name);
}

bool sl_slot_true(sl_obj_t *o, const char *name)
{
    return slot_true(o, name);
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

bool sl_realized(const sl_obj_t *o)
{
    return realized(o);
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
    sl_anim_flush();   /* a PHP call: what plain C left to release goes now */
    if (p->def->how == HOW_JUMP) {
        sl_scroll_sync(o);   /* both slots at the position drawn: the JUMP keeps the other axis where it is */
    }
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
       immutable) changes nothing: no SET, so nothing is redrawn and the compositor is not woken. Not for an element
       a tween or ease() touched: the screen may show another value (a stopped tween), and the write puts it right. */
    bool same = Z_TYPE(old) != IS_ARRAY && zend_is_identical(&old, slot) && !o->animated;
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
    bool changed = !zend_is_identical(&old, slot);
    zval_ptr_dtor(&old);
    if (p->def->how == HOW_JUMP) {
        return sl_scroll_jumped(o) ? SUCCESS : FAILURE;   /* always: the slot may have been behind the screen */
    }
    if (p->def->how == HOW_NONE) {
        return SUCCESS;   /* $holdMs: read at the next Press */
    }
    if (p->def->how == HOW_CONFIG) {
        if (!changed) {
            return SUCCESS;   /* a CONFIG of the same set-up changes nothing */
        }
        return sl_scroll_configured(o, strcmp(p->def->name, "indicators") != 0) ? SUCCESS : FAILURE;
    }
    /* A canvas resized: the PHP buffer is cleared here, and the engine clears its own copy when the SET of W or H
       arrives (scene.h), so no pixels need to be sent again. */
    if (o->base == SL_CANVAS && (p->def->prop == SLATE_PROP_W || p->def->prop == SLATE_PROP_H)) {
        if (!sl_canvas_resize(o, (int)slot_num(o, "w"), (int)slot_num(o, "h"))) {
            return FAILURE;
        }
    }
    if (o->tweens) {
        sl_anim_written(o, p->def->prop);   /* a pending tween no longer moves it */
    }
    if (!same && realized(o) && !push_set(o, p, &v)) {
        return FAILURE;
    }
    if (p->def->type == PT_TOUCH && Z_TYPE_P(slot) == IS_TRUE) {
        sl_touch_note(o);
    }
    if (changed && (p->def->type == PT_TOUCH || p->def->prop == SLATE_PROP_VISIBLE)) {
        sl_touch_recheck();   /* the captured element (or an ancestor) hidden, or no longer touchable */
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
    sl_obj_t *o = sl_obj(obj);
    if (o->base == SL_SCROLL) {
        const sl_prop_t *p = find_prop(o, name);
        if (p && p->def->how == HOW_JUMP) {
            /* the position drawn: never cached (a cached read would skip this handler) */
            sl_scroll_sync(o);
            return zend_std_read_property(obj, name, type, NULL, rv);
        }
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
    sl_touch_gc(o, buf);   /* the callbacks of on() (a closure may hold the element) */
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
                sl_obj_t *c = sl_obj(Z_OBJ_P(zv));
                c->parent = NULL;
                if (c->tweens) {
                    sl_anim_removed(c);   /* detached for good unless added again: its tweens stop */
                }
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
    if (o->sc) {
        sl_scroll_free(o);
    }
    sl_touch_forget(o);
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
        ZVAL_BOOL(out, d->prop == SLATE_PROP_VISIBLE || d->how == HOW_CONFIG);   /* a Scroll's bounce, indicators */
        return;
    case PT_CLIPON:
        ZVAL_TRUE(out);
        return;
    case PT_VIEW:
        ZVAL_LONG(out, 1);
        return;
    case PT_AXIS:
        ZVAL_OBJ_COPY(out, zend_enum_get_case_cstr(sl_scroll_axis_ce, "Both"));
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
    if (o->base == SL_GROUP || o->base == SL_SCROLL || o->base == SL_SCENE) {
        zend_hash_init(&o->children, 4, NULL, ZVAL_PTR_DTOR, 0);
        o->has_children = true;
    }
    if (o->base == SL_SCROLL) {
        sl_scroll_init(o);
    }
    for (int i = 0; i < s_nprops[o->base]; i++) {
        const sl_prop_t *p = &s_props[o->base][i];
        if (p->def->type == PT_TOUCH || p->def->type == PT_HITPAD || p->def->type == PT_HOLDMS) {
            continue;   /* the class's default (a subclass may declare its own) */
        }
        zval *slot = prop_slot(o, p);
        zval_ptr_dtor(slot);
        default_of(o->base, p->def, slot);
    }
    if (o->base == SL_IMAGE) {
        sl_image_init(o);
    }
    if (o->base != SL_SCENE && slot_true(o, "touchable")) {
        sl_touch_note(o);
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
    if (o->base == SL_SCROLL && !sl_scroll_realize_config(o)) {   /* it is a scroll before its properties */
        return false;
    }
    for (int i = 0; i < s_nprops[o->base]; i++) {
        const sl_prop_t *p = &s_props[o->base][i];
        if (p->def->how != HOW_SET) {
            continue;   /* in the CONFIG, or the JUMP after the children */
        }
        slate_value_t v;
        slate_paint_grad_t grad;
        to_value(o, p, prop_slot(o, p), &v, &grad, false);
        if ((p->def->type == PT_TOUCH && !v.v.b) || (p->def->type == PT_HITPAD && v.v.num == 0)) {
            continue;   /* the engine's default after NEW: no command for an element that takes no touch */
        }
        if (!push_set(o, p, &v)) {
            return false;
        }
    }
    if (o->base == SL_CANVAS && o->px && !sl_canvas_send(o, 0, 0, o->cw, o->ch)) {
        return false;
    }
    if (o->ease_mask) {   /* after the properties: they jump to their values, later writes slide */
        slate_ease_spec_t es = { .mask = o->ease_mask, .ms = o->ease_ms, .ease = o->ease };
        slate_cmd_t ec = { .kind = SLATE_CMD_EASE, .id = o->id, .data = &es };
        if (!sl_push(&ec)) {
            return false;
        }
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
    if (o->base == SL_SCROLL && !sl_scroll_realize_position(o)) {   /* clamped to the content of its children */
        return false;
    }
    return sl_anim_realized(o);   /* the tweens made before it was drawn */
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

static void children_remove(sl_obj_t *o, zval *args, uint32_t argc);

void sl_children_remove(sl_obj_t *o, zval *args, uint32_t argc)
{
    children_remove(o, args, argc);
    sl_touch_recheck();   /* the captured element (or an ancestor) taken out of the scene */
}

static void children_remove(sl_obj_t *o, zval *args, uint32_t argc)
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
        if (c->tweens) {
            sl_anim_removed(c);   /* its tweens (running or pending) stop unless it is back in a group at the delivery */
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
static const char *const scroll_args[] = { "x", "y", "w", "h", "axis", "bounce", "snapX", "snapY", "snapMax",
                                           "indicators", "contentW", "contentH", "opacity", "cache" };

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

/* In Scroll's method table (php_slate_scroll.c). */
PHP_METHOD(SlateScroll, __construct)
{
    construct(INTERNAL_FUNCTION_PARAM_PASSTHRU, scroll_args, 0, 14);
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

/* ---- touch: on(), off(), the empty methods ---- */

PHP_METHOD(SlateElement, on)
{
    zend_object *type;
    zval *fn;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJ_OF_CLASS(type, sl_touch_type_ce)
        Z_PARAM_OBJECT_OF_CLASS(fn, zend_ce_closure)
    ZEND_PARSE_PARAMETERS_END();
    sl_anim_flush();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    bool was = sl_touch_has_cb(o);
    sl_touch_set_cb(o, sl_touch_kind_of(type), fn);
    sl_touch_note(o);
    if (!was && !slot_true(o, "touchable") && realized(o)) {
        const sl_prop_t *p = find_prop_c(o, "touchable");
        slate_value_t v = slate_v_bool(true);
        if (p && !push_set(o, p, &v)) {
            RETURN_THROWS();
        }
    }
    RETURN_OBJ_COPY(&o->std);
}

PHP_METHOD(SlateElement, off)
{
    zend_object *type;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJ_OF_CLASS(type, sl_touch_type_ce)
    ZEND_PARSE_PARAMETERS_END();
    sl_anim_flush();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    bool was = sl_touch_has_cb(o);
    sl_touch_set_cb(o, sl_touch_kind_of(type), NULL);
    if (was && !sl_touch_has_cb(o) && !slot_true(o, "touchable")) {
        if (realized(o)) {
            const sl_prop_t *p = find_prop_c(o, "touchable");
            slate_value_t v = slate_v_bool(false);
            if (p && !push_set(o, p, &v)) {
                RETURN_THROWS();
            }
        }
        sl_touch_recheck();   /* the captured element no longer touchable */
        if (EG(exception)) {
            RETURN_THROWS();
        }
    }
    RETURN_OBJ_COPY(&o->std);
}

/* onPress() .. onCancel(): empty, for a subclass to override (php_slate_touch.c skips these when not overridden). */
#define SL_TOUCH_METHOD(name)                     \
    PHP_METHOD(SlateElement, name)                \
    {                                             \
        zend_object *t;                           \
        ZEND_PARSE_PARAMETERS_START(1, 1)         \
            Z_PARAM_OBJ_OF_CLASS(t, sl_touch_ce)  \
        ZEND_PARSE_PARAMETERS_END();              \
        (void)t;                                  \
    }
SL_TOUCH_METHOD(onPress)
SL_TOUCH_METHOD(onMove)
SL_TOUCH_METHOD(onRelease)
SL_TOUCH_METHOD(onTap)
SL_TOUCH_METHOD(onHold)
SL_TOUCH_METHOD(onCancel)

/* ---- animate(), ease() ---- */

/* The animatable properties (design §1.2): numbers and solid colours. */
static bool animatable(const sl_obj_t *o, const sl_prop_t *p)
{
    int b = o->base;
    switch (p->def->prop) {
    case SLATE_PROP_X:
    case SLATE_PROP_Y:
    case SLATE_PROP_OPACITY:
        return true;
    case SLATE_PROP_W:
    case SLATE_PROP_H:
        return b == SL_GROUP || b == SL_SCROLL || b == SL_ROUNDRECT;   /* a Canvas's size is its buffer: not
                                                                          animatable */
    case SLATE_PROP_BLUR:
        return b == SL_GROUP || b == SL_SCROLL;
    case SLATE_PROP_RADIUS:
        return b == SL_ROUNDRECT;
    case SLATE_PROP_BORDER:
    case SLATE_PROP_FILL:
    case SLATE_PROP_BORDER_COLOR:
        return b == SL_ROUNDRECT || b == SL_CIRCLE;
    case SLATE_PROP_R:
        return b == SL_CIRCLE || b == SL_ARC;
    case SLATE_PROP_FROM:
    case SLATE_PROP_TO:
        return b == SL_ARC;
    case SLATE_PROP_WIDTH:
        return b == SL_ARC || b == SL_LINE;
    case SLATE_PROP_COLOR:
        return b == SL_ARC || b == SL_LINE || b == SL_TEXT;
    case SLATE_PROP_X1:
    case SLATE_PROP_Y1:
        return b == SL_LINE;
    default:
        return false;
    }
}

static void not_animatable(const sl_obj_t *o, const char *name)
{
    zend_value_error("%s::$%s cannot be animated", ZSTR_VAL(o->std.ce->name), name);
}

static bool is_gradient(const zval *v)
{
    ZVAL_DEREF(v);
    return Z_TYPE_P(v) == IS_OBJECT && instanceof_function(Z_OBJCE_P(v), sl_gradient_ce);
}

/* The property named by a key of animate() (or a name of ease()), animatable for o's class; NULL with the
   \ValueError. */
static const sl_prop_t *anim_prop(sl_obj_t *o, zend_string *key, zend_ulong idx)
{
    const sl_prop_t *p = key ? find_prop(o, key) : NULL;
    if (p && o->base == SL_SCROLL
        && (p->def->how == HOW_JUMP || p->def->prop == SLATE_PROP_CONTENT_W || p->def->prop == SLATE_PROP_CONTENT_H
            || (p->def->how == HOW_CONFIG && p->def->type == PT_PIXELS))) {
        /* the position, the content size, the snap steps and $snapMax: the physics owns them */
        zend_value_error("%s::$%s cannot be animated; use scrollTo()", ZSTR_VAL(o->std.ce->name), p->def->name);
        return NULL;
    }
    if (!p || !animatable(o, p)) {
        if (key) {
            not_animatable(o, ZSTR_VAL(key));
        } else {
            char num[24];
            snprintf(num, sizeof num, ZEND_ULONG_FMT, idx);
            not_animatable(o, num);
        }
        return NULL;
    }
    return p;
}

/* Puts back the slots of ps[0..n) from old (their copies), freeing the values written. */
static void slots_back(sl_obj_t *o, const sl_prop_t *const *ps, zval *old, int n)
{
    for (int i = 0; i < n; i++) {
        zval *slot = prop_slot(o, ps[i]);
        zval tmp;
        ZVAL_COPY_VALUE(&tmp, slot);
        ZVAL_COPY_VALUE(slot, &old[i]);
        zval_ptr_dtor(&tmp);
    }
}

bool sl_anim_collect(sl_obj_t *o, HashTable *props, bool back, slate_tween_spec_t *spec)
{
    if (zend_hash_num_elements(props) == 0) {
        zend_value_error("props must name at least one property");
        return false;
    }
    const sl_prop_t *ps[SLATE_TWEEN_PROPS_MAX];
    zval *vals[SLATE_TWEEN_PROPS_MAX];
    int k = 0;
    zend_string *key;
    zend_ulong idx;
    zval *v;
    /* the names and the gradients first: nothing is written before they are all right */
    ZEND_HASH_FOREACH_KEY_VAL(props, idx, key, v) {
        const sl_prop_t *p = anim_prop(o, key, idx);
        if (!p) {
            return false;
        }
        ZVAL_DEREF(v);
        if ((p->def->type == PT_PAINT || p->def->type == PT_NPAINT) && (is_gradient(v) || is_gradient(prop_slot(o, p)))) {
            zend_value_error("%s::$%s cannot be animated: it is a gradient", ZSTR_VAL(o->std.ce->name), p->def->name);
            return false;
        }
        if ((p->def->type == PT_NCOLOR || p->def->type == PT_NPAINT)
            && (Z_TYPE_P(v) == IS_NULL || Z_TYPE_P(prop_slot(o, p)) == IS_NULL)) {
            /* no fill or no border: nothing to blend from or to (design §1.2) */
            zend_value_error("%s::$%s cannot be animated: it is null", ZSTR_VAL(o->std.ce->name), p->def->name);
            return false;
        }
        if (k == SLATE_TWEEN_PROPS_MAX) {   /* never: a class has at most 9 animatable properties */
            zend_value_error("props can name at most %d properties", SLATE_TWEEN_PROPS_MAX);
            return false;
        }
        ps[k] = p;
        vals[k] = v;
        k++;
    } ZEND_HASH_FOREACH_END();
    /* then each value as a write of it: PHP's type check (and coercion), the range, the rules between properties;
       the old values come back on the first refusal */
    zval old[SLATE_TWEEN_PROPS_MAX];
    int w = 0;
    bool ok = true;
    for (int i = 0; i < k && ok; i++) {
        zval *slot = prop_slot(o, ps[i]);
        ZVAL_COPY(&old[i], slot);
        w++;
        zend_std_write_property(&o->std, ps[i]->name, vals[i], NULL);
        slate_value_t sv;
        slate_paint_grad_t grad;
        ok = !EG(exception) && to_value(o, ps[i], slot, &sv, &grad, true);
    }
    for (int i = 0; i < k && ok; i++) {
        ok = cross_check(o, ps[i]);
    }
    if (ok) {
        for (int i = 0; i < k; i++) {
            slate_value_t sv;
            slate_paint_grad_t grad;
            to_value(o, ps[i], prop_slot(o, ps[i]), &sv, &grad, false);
            slate_value_t fv;
            to_value(o, ps[i], &old[i], &fv, &grad, false);   /* the value before: a pending tween sets it first */
            slate_tween_prop_t *tp = &spec->props[i];
            memset(tp, 0, sizeof *tp);
            tp->prop = ps[i]->def->prop;
            if (sv.type == SLATE_VAL_COLOR) {
                tp->type = SLATE_VAL_COLOR;
                tp->to.argb = sv.v.argb;
                tp->from.argb = fv.v.argb;
            } else if (sv.type == SLATE_VAL_PAINT) {
                tp->type = SLATE_VAL_COLOR;   /* solid: gradients and null were refused above */
                tp->to.argb = sv.v.paint.argb;
                tp->from.argb = fv.v.paint.argb;
            } else {
                tp->type = SLATE_VAL_NUM;
                tp->to.num = sv.v.num;
                tp->from.num = fv.v.num;
            }
        }
        spec->n = (uint8_t)k;
        o->animated = true;
    }
    if (!ok || back) {
        slots_back(o, ps, old, w);
    } else {
        for (int i = 0; i < w; i++) {
            zval_ptr_dtor(&old[i]);
        }
    }
    return ok;
}

PHP_METHOD(SlateElement, animate)
{
    HashTable *props;
    zend_long ms = 300, delay = 0, repeat = 0;
    zend_object *ease = NULL;
    bool yoyo = false;
    ZEND_PARSE_PARAMETERS_START(1, 6)
        Z_PARAM_ARRAY_HT(props)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
        Z_PARAM_OBJ_OF_CLASS(ease, sl_ease_ce)
        Z_PARAM_LONG(delay)
        Z_PARAM_LONG(repeat)
        Z_PARAM_BOOL(yoyo)
    ZEND_PARSE_PARAMETERS_END();
    if (!sl_anim_times(ms, delay, repeat)) {
        RETURN_THROWS();
    }
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    slate_tween_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.ms = (int32_t)ms;
    spec.delay_ms = (int32_t)delay;
    spec.repeat = (int32_t)repeat;
    spec.yoyo = yoyo;
    spec.ease = sl_ease_of(ease);
    /* with yoyo and an odd number of repeats the last run comes back: it ends where it starts */
    if (!sl_anim_collect(o, props, yoyo && repeat > 0 && (repeat & 1), &spec)
        || !sl_tween_make(return_value, o, &spec, NULL)) {
        RETURN_THROWS();
    }
}

PHP_METHOD(SlateElement, ease)
{
    HashTable *props;
    zend_long ms = 300;
    zend_object *ease = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_ARRAY_HT(props)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
        Z_PARAM_OBJ_OF_CLASS(ease, sl_ease_ce)
    ZEND_PARSE_PARAMETERS_END();
    if (!sl_anim_times(ms, 0, 0)) {
        RETURN_THROWS();
    }
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    uint32_t mask = 0;
    zval *v;
    ZEND_HASH_FOREACH_VAL(props, v) {
        ZVAL_DEREF(v);
        const sl_prop_t *p;
        if (Z_TYPE_P(v) == IS_STRING) {
            p = anim_prop(o, Z_STR_P(v), 0);
        } else if (Z_TYPE_P(v) == IS_LONG) {
            p = anim_prop(o, NULL, (zend_ulong)Z_LVAL_P(v));
        } else {
            zend_argument_type_error(1, "must contain only property names, %s given", zend_zval_value_name(v));
            RETURN_THROWS();
        }
        if (!p) {
            RETURN_THROWS();
        }
        mask |= 1u << p->def->prop;   /* animatable: below 32 (php_slate_anim.c) */
    } ZEND_HASH_FOREACH_END();
    o->ease_mask = mask;
    o->ease_ms = (int32_t)ms;
    o->ease = sl_ease_of(ease);
    o->animated = o->animated || mask != 0;
    if (realized(o)) {
        slate_ease_spec_t es = { .mask = mask, .ms = o->ease_ms, .ease = o->ease };
        slate_cmd_t c = { .kind = SLATE_CMD_EASE, .id = o->id, .data = &es };
        if (!sl_push(&c)) {
            RETURN_THROWS();
        }
    }
    RETURN_OBJ_COPY(&o->std);
}

/* ---- arginfo ---- */

#define T_NUM (MAY_BE_LONG | MAY_BE_DOUBLE)

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_element_set, 0, 0, IS_STATIC, 0)
    ZEND_ARG_VARIADIC_TYPE_INFO(0, props, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_element_animate, 0, 1, Baremetal\\Slate\\Tween, 0)
    ZEND_ARG_TYPE_INFO(0, props, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ms, IS_LONG, 0, "300")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, ease, Baremetal\\Slate\\Ease, 0, "\\Baremetal\\Slate\\Ease::OutCubic")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, delay, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, repeat, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, yoyo, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_element_ease, 0, 1, IS_STATIC, 0)
    ZEND_ARG_TYPE_INFO(0, props, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ms, IS_LONG, 0, "300")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, ease, Baremetal\\Slate\\Ease, 0, "\\Baremetal\\Slate\\Ease::OutCubic")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_element_on, 0, 2, IS_STATIC, 0)
    ZEND_ARG_OBJ_INFO(0, type, Baremetal\\Slate\\TouchType, 0)
    ZEND_ARG_OBJ_INFO(0, fn, Closure, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_element_off, 0, 1, IS_STATIC, 0)
    ZEND_ARG_OBJ_INFO(0, type, Baremetal\\Slate\\TouchType, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_element_touch, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, t, Baremetal\\Slate\\Touch, 0)
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
    PHP_ME(SlateElement, animate, arginfo_element_animate, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, ease, arginfo_element_ease, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, on, arginfo_element_on, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, off, arginfo_element_off, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, onPress, arginfo_element_touch, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, onMove, arginfo_element_touch, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, onRelease, arginfo_element_touch, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, onTap, arginfo_element_touch, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, onHold, arginfo_element_touch, ZEND_ACC_PUBLIC)
    PHP_ME(SlateElement, onCancel, arginfo_element_touch, ZEND_ACC_PUBLIC)
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
    case PT_TOUCH:
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
    case PT_AXIS:
        return (zend_type)ZEND_TYPE_INIT_CLASS(s_axis_name, 0, 0);
    case PT_CLIPON:
        return (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0);
    case PT_BLUR:
    case PT_PIXELS:
    case PT_CSIZE:
    case PT_HITPAD:
    case PT_HOLDMS:
        return (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0);
    default:   /* COORD, SIZE, ANGLE, COLOR, VIEW, SPOS */
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
    s_axis_name = zend_string_init_interned("Baremetal\\Slate\\ScrollAxis", sizeof("Baremetal\\Slate\\ScrollAxis") - 1, 1);

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
    declare(sl_element_ce, common_defs, COMMON_DRAWN);
    /* the touch properties, with their defaults (Scroll redeclares $touchable with true, below) */
    for (int i = COMMON_DRAWN; i < N(common_defs); i++) {
        zval dv;
        if (common_defs[i].type == PT_TOUCH) {
            ZVAL_FALSE(&dv);
        } else {
            ZVAL_LONG(&dv, common_defs[i].type == PT_HOLDMS ? 500 : 0);
        }
        zend_declare_typed_property(sl_element_ce,
                                    zend_string_init_interned(common_defs[i].name, strlen(common_defs[i].name), 1),
                                    &dv, ZEND_ACC_PUBLIC, NULL, type_of(common_defs[i].type));
    }

    sl_group_ce = leaf("Baremetal\\Slate\\Group", group_methods, group_defs, N(group_defs), false);
    sl_scroll_axis_register();
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Scroll", sl_scroll_method_table());
    sl_scroll_ce = zend_register_internal_class_ex(&ce, sl_group_ce);   /* not final: a list of an app extends it */
    sl_scroll_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    declare(sl_scroll_ce, scroll_defs + SCROLL_INHERITED, N(scroll_defs) - SCROLL_INHERITED);
    /* a Scroll takes the touch by itself: $touchable true by default (redeclared: the same slot as Element's) */
    {
        zval dv;
        ZVAL_TRUE(&dv);
        zend_declare_typed_property(sl_scroll_ce, zend_string_init_interned("touchable", sizeof("touchable") - 1, 1),
                                    &dv, ZEND_ACC_PUBLIC, NULL, type_of(PT_TOUCH));
    }
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
    s_base_ce[SL_SCROLL] = sl_scroll_ce;

    table(SL_GROUP, group_defs, N(group_defs));
    table(SL_ROUNDRECT, roundrect_defs, N(roundrect_defs));
    table(SL_CIRCLE, circle_defs, N(circle_defs));
    table(SL_ARC, arc_defs, N(arc_defs));
    table(SL_LINE, line_defs, N(line_defs));
    table(SL_TEXT, text_defs, N(text_defs));
    table(SL_CANVAS, canvas_defs, N(canvas_defs));
    table(SL_IMAGE, image_defs, N(image_defs));
    table(SL_SCROLL, scroll_defs, N(scroll_defs));
}

void sl_elements_scene(zend_class_entry *scene_ce)
{
    s_base_ce[SL_SCENE] = scene_ce;
    scene_ce->create_object = sl_obj_create;
    table(SL_SCENE, NULL, 0);
}
