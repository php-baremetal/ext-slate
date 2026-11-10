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
 * The value classes of Slate: the enums Caps and Align, the gradients (abstract Gradient, LinearGradient,
 * RadialGradient) and Shadow. Gradients and shadows are immutable (readonly properties); their C form lives in
 * the object, and the command buffer copies it at every push. A gradient or a shadow cannot be built when its
 * effect is left out of the slate-effects option.
 */
#include <math.h>
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"

#include "php_slate.h"
#include "slate_config.h"

zend_class_entry *sl_caps_ce, *sl_align_ce, *sl_gradient_ce, *sl_linear_ce, *sl_radial_ce, *sl_shadow_ce;

static zend_object_handlers sl_grad_handlers, sl_shadow_handlers;

typedef struct {
    slate_stop_t stops[SLATE_GRADIENT_STOPS_MAX];
    int ns;
    bool radial;
    float angle, cx, cy, r;
    zend_object std;
} sl_grad_t;

typedef struct {
    int dx, dy, blur;
    uint32_t argb;
    zend_object std;
} sl_shadow_t;

static sl_grad_t *grad_of(zend_object *o)
{
    return (sl_grad_t *)((char *)o - XtOffsetOf(sl_grad_t, std));
}

static sl_shadow_t *shadow_of(zend_object *o)
{
    return (sl_shadow_t *)((char *)o - XtOffsetOf(sl_shadow_t, std));
}

void sl_not_built(const char *effect)
{
    zend_value_error("%s is not built in: add it to slate-effects in composer.json", effect);
}

bool sl_color_of(const zval *v, uint32_t *argb)
{
    double d;
    if (Z_TYPE_P(v) == IS_LONG) {
#if SIZEOF_ZEND_LONG > 4
        if (Z_LVAL_P(v) < 0 || Z_LVAL_P(v) > (zend_long)0xFFFFFFFF) {
            return false;
        }
#else
        if (Z_LVAL_P(v) < 0) {
            return false;
        }
#endif
        d = (double)Z_LVAL_P(v);
    } else if (Z_TYPE_P(v) == IS_DOUBLE) {
        d = Z_DVAL_P(v);
        if (!(d >= 0.0 && d <= 4294967295.0) || d != floor(d)) {
            return false;
        }
    } else {
        return false;
    }
    uint32_t c = (uint32_t)d;
    *argb = c <= 0xFFFFFFu ? (0xFF000000u | c) : c;   /* no alpha byte: opaque */
    return true;
}

int sl_blur_radius(zend_long blur)
{
    if (blur <= 0) {
        return 0;
    }
    zend_long r = (blur + 1) / 3;   /* round(blur / 3) */
    if (r < 1) {
        r = 1;
    }
    return r > SLATE_BLUR_RADIUS_MAX ? SLATE_BLUR_RADIUS_MAX : (int)r;
}

/* ---- the enums ---- */

static void enum_case(zend_class_entry *ce, const char *name, const char *value)
{
    zval v;
    ZVAL_STR(&v, zend_string_init_interned(value, strlen(value), 1));
    zend_enum_add_case_cstr(ce, name, &v);
}

/* ---- readonly slots ---- */

static zval *slot(zend_object *obj, const char *name)
{
    zend_property_info *info = zend_hash_str_find_ptr(&obj->ce->properties_info, name, strlen(name));
    return info ? OBJ_PROP(obj, info->offset) : NULL;
}

static void init_double(zend_object *obj, const char *name, double v)
{
    zval *z = slot(obj, name);
    if (z) {
        zval_ptr_dtor(z);
        ZVAL_DOUBLE(z, v);
    }
}

static void init_long(zend_object *obj, const char *name, zend_long v)
{
    zval *z = slot(obj, name);
    if (z) {
        zval_ptr_dtor(z);
        ZVAL_LONG(z, v);
    }
}

static void init_zval(zend_object *obj, const char *name, zval *v)
{
    zval *z = slot(obj, name);
    if (z) {
        zval_ptr_dtor(z);
        ZVAL_COPY(z, v);
    }
}

static void declare(zend_class_entry *ce, const char *name, zend_type type)
{
    zval undef;
    ZVAL_UNDEF(&undef);
    zend_declare_typed_property(ce, zend_string_init_interned(name, strlen(name), 1), &undef,
                                ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL, type);
}

/* ---- gradients ---- */

static zend_object *grad_create(zend_class_entry *ce)
{
    sl_grad_t *g = zend_object_alloc(sizeof(sl_grad_t), ce);
    memset(g, 0, XtOffsetOf(sl_grad_t, std));
    zend_object_std_init(&g->std, ce);
    object_properties_init(&g->std, ce);
    g->std.handlers = &sl_grad_handlers;
    return &g->std;
}

static bool finite_in(double v, double lo, double hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

/* stops: a list of [position, colour] or [position, colour, alpha] entries, positions 0..1, alpha 0..1 (it replaces
   the colour's alpha: [1.0, 0xFFFFFF, 0.0] is fully transparent white), 2 to 16 of them; sorted by position here. */
static bool parse_stops(sl_grad_t *g, HashTable *stops, const char *cls)
{
    uint32_t n = zend_hash_num_elements(stops);
    if (n < 2 || n > SLATE_GRADIENT_STOPS_MAX) {
        zend_value_error("%s: $stops must have between 2 and %d stops, got %u", cls, SLATE_GRADIENT_STOPS_MAX,
                         (unsigned)n);
        return false;
    }
    int i = 0;
    zval *st;
    ZEND_HASH_FOREACH_VAL(stops, st) {
        ZVAL_DEREF(st);
        zval *at = NULL, *color = NULL, *alpha = NULL;
        uint32_t cnt = Z_TYPE_P(st) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(st)) : 0;
        if (cnt == 2 || cnt == 3) {
            at = zend_hash_index_find(Z_ARRVAL_P(st), 0);
            color = zend_hash_index_find(Z_ARRVAL_P(st), 1);
            if (cnt == 3) {
                alpha = zend_hash_index_find(Z_ARRVAL_P(st), 2);
                if (!alpha) {
                    at = NULL;   /* not a list: refused below */
                } else {
                    ZVAL_DEREF(alpha);
                }
            }
        }
        if (at) {
            ZVAL_DEREF(at);
        }
        if (color) {
            ZVAL_DEREF(color);
        }
        uint32_t argb;
        if (!at || !color || (Z_TYPE_P(at) != IS_LONG && Z_TYPE_P(at) != IS_DOUBLE)) {
            zend_value_error("%s: each of $stops must be [position, color] or [position, color, alpha], e.g. "
                             "[0.0, 0x202830]", cls);
            return false;
        }
        double pos = zval_get_double(at);
        if (!finite_in(pos, 0.0, 1.0)) {
            zend_value_error("%s: a position in $stops must be between 0 and 1", cls);
            return false;
        }
        if (!sl_color_of(color, &argb)) {
            zend_value_error("%s: a color in $stops must be 0xRRGGBB or 0xAARRGGBB", cls);
            return false;
        }
        if (alpha) {
            double a;
            if ((Z_TYPE_P(alpha) != IS_LONG && Z_TYPE_P(alpha) != IS_DOUBLE)
                || !finite_in(a = zval_get_double(alpha), 0.0, 1.0)) {
                zend_value_error("%s: an alpha in $stops must be between 0 and 1", cls);
                return false;
            }
            argb = (argb & 0xFFFFFFu) | ((uint32_t)(a * 255.0 + 0.5) << 24);
        }
        /* insertion by position, stable */
        int j = i;
        while (j > 0 && g->stops[j - 1].at > (float)pos) {
            g->stops[j] = g->stops[j - 1];
            j--;
        }
        g->stops[j].at = (float)pos;
        g->stops[j].argb = argb;
        i++;
    } ZEND_HASH_FOREACH_END();
    g->ns = i;
    return true;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_linear_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, stops, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, angle, IS_DOUBLE, 0, "0.0")
ZEND_END_ARG_INFO()

PHP_METHOD(SlateLinearGradient, __construct)
{
    zval *stops;
    double angle = 0;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ARRAY(stops)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(angle)
    ZEND_PARSE_PARAMETERS_END();
    if (!SLATE_HAS_GRADIENTS) {
        sl_not_built("gradients");
        RETURN_THROWS();
    }
    sl_grad_t *g = grad_of(Z_OBJ_P(ZEND_THIS));
    if (slot(&g->std, "stops") && Z_TYPE_P(slot(&g->std, "stops")) != IS_UNDEF) {
        zend_throw_error(NULL, "Cannot modify readonly property Baremetal\\Slate\\LinearGradient::$stops");
        RETURN_THROWS();
    }
    if (!finite_in(angle, -1e6, 1e6)) {
        zend_argument_value_error(2, "must be a finite number of degrees");
        RETURN_THROWS();
    }
    if (!parse_stops(g, Z_ARRVAL_P(stops), "LinearGradient")) {
        RETURN_THROWS();
    }
    g->radial = false;
    g->angle = (float)angle;
    init_zval(&g->std, "stops", stops);
    init_double(&g->std, "angle", angle);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_radial_ctor, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, stops, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cx, IS_DOUBLE, 0, "0.5")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cy, IS_DOUBLE, 0, "0.5")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, r, IS_DOUBLE, 0, "0.5")
ZEND_END_ARG_INFO()

PHP_METHOD(SlateRadialGradient, __construct)
{
    zval *stops;
    double cx = 0.5, cy = 0.5, r = 0.5;
    ZEND_PARSE_PARAMETERS_START(1, 4)
        Z_PARAM_ARRAY(stops)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(cx)
        Z_PARAM_DOUBLE(cy)
        Z_PARAM_DOUBLE(r)
    ZEND_PARSE_PARAMETERS_END();
    if (!SLATE_HAS_GRADIENTS) {
        sl_not_built("gradients");
        RETURN_THROWS();
    }
    sl_grad_t *g = grad_of(Z_OBJ_P(ZEND_THIS));
    if (slot(&g->std, "stops") && Z_TYPE_P(slot(&g->std, "stops")) != IS_UNDEF) {
        zend_throw_error(NULL, "Cannot modify readonly property Baremetal\\Slate\\RadialGradient::$stops");
        RETURN_THROWS();
    }
    if (!finite_in(cx, -100, 100)) {
        zend_argument_value_error(2, "must be between -100 and 100 (a fraction of the box)");
        RETURN_THROWS();
    }
    if (!finite_in(cy, -100, 100)) {
        zend_argument_value_error(3, "must be between -100 and 100 (a fraction of the box)");
        RETURN_THROWS();
    }
    if (!finite_in(r, 0, 100) || r <= 0) {
        zend_argument_value_error(4, "must be greater than 0 and at most 100 (a fraction of the box)");
        RETURN_THROWS();
    }
    if (!parse_stops(g, Z_ARRVAL_P(stops), "RadialGradient")) {
        RETURN_THROWS();
    }
    g->radial = true;
    g->cx = (float)cx;
    g->cy = (float)cy;
    g->r = (float)r;
    init_zval(&g->std, "stops", stops);
    init_double(&g->std, "cx", cx);
    init_double(&g->std, "cy", cy);
    init_double(&g->std, "r", r);
}

bool sl_gradient_paint(zend_object *obj, slate_paint_t *out, slate_paint_grad_t *grad)
{
    sl_grad_t *g = grad_of(obj);
    if (g->ns < 2) {
        return false;   /* a user subclass of Gradient, or a gradient never constructed: no stops */
    }
    *grad = (slate_paint_grad_t){ g->stops, g->ns, g->radial, g->angle, g->cx, g->cy, g->r };
    out->argb = 0;
    out->grad = grad;
    return true;
}

/* ---- Shadow ---- */

static zend_object *shadow_create(zend_class_entry *ce)
{
    sl_shadow_t *s = zend_object_alloc(sizeof(sl_shadow_t), ce);
    memset(s, 0, XtOffsetOf(sl_shadow_t, std));
    zend_object_std_init(&s->std, ce);
    object_properties_init(&s->std, ce);
    s->std.handlers = &sl_shadow_handlers;
    return &s->std;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_shadow_ctor, 0, 0, 4)
    ZEND_ARG_TYPE_INFO(0, dx, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, dy, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, blur, IS_LONG, 0)
    ZEND_ARG_TYPE_MASK(0, color, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
ZEND_END_ARG_INFO()

PHP_METHOD(SlateShadow, __construct)
{
    zend_long dx, dy, blur;
    zval *color;
    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_LONG(dx)
        Z_PARAM_LONG(dy)
        Z_PARAM_LONG(blur)
        Z_PARAM_NUMBER(color)
    ZEND_PARSE_PARAMETERS_END();
    if (!SLATE_HAS_SHADOWS) {
        sl_not_built("shadows");
        RETURN_THROWS();
    }
    sl_shadow_t *s = shadow_of(Z_OBJ_P(ZEND_THIS));
    if (slot(&s->std, "dx") && Z_TYPE_P(slot(&s->std, "dx")) != IS_UNDEF) {
        zend_throw_error(NULL, "Cannot modify readonly property Baremetal\\Slate\\Shadow::$dx");
        RETURN_THROWS();
    }
    if (dx < -32767 || dx > 32767) {
        zend_argument_value_error(1, "must be between -32767 and 32767");
        RETURN_THROWS();
    }
    if (dy < -32767 || dy > 32767) {
        zend_argument_value_error(2, "must be between -32767 and 32767");
        RETURN_THROWS();
    }
    if (blur < 0 || blur > 3 * SLATE_BLUR_RADIUS_MAX) {
        zend_argument_value_error(3, "must be between 0 and %d", 3 * SLATE_BLUR_RADIUS_MAX);
        RETURN_THROWS();
    }
    uint32_t argb;
    if (!sl_color_of(color, &argb)) {
        zend_argument_value_error(4, "must be a color, 0xRRGGBB or 0xAARRGGBB");
        RETURN_THROWS();
    }
    s->dx = (int)dx;
    s->dy = (int)dy;
    s->blur = (int)blur;
    s->argb = argb;
    init_long(&s->std, "dx", dx);
    init_long(&s->std, "dy", dy);
    init_long(&s->std, "blur", blur);
    init_zval(&s->std, "color", color);
}

slate_value_t sl_shadow_value(zend_object *obj)
{
    if (!obj) {
        return slate_v_shadow(0, 0, 0, 0);
    }
    sl_shadow_t *s = shadow_of(obj);
    return slate_v_shadow(s->dx, s->dy, sl_blur_radius(s->blur), s->argb);
}

static const zend_function_entry linear_methods[] = {
    PHP_ME(SlateLinearGradient, __construct, arginfo_linear_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry radial_methods[] = {
    PHP_ME(SlateRadialGradient, __construct, arginfo_radial_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry shadow_methods[] = {
    PHP_ME(SlateShadow, __construct, arginfo_shadow_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void sl_effects_register(void)
{
    sl_caps_ce = zend_register_internal_enum("Baremetal\\Slate\\Caps", IS_STRING, NULL);
    enum_case(sl_caps_ce, "Butt", "butt");
    enum_case(sl_caps_ce, "Round", "round");

    sl_align_ce = zend_register_internal_enum("Baremetal\\Slate\\Align", IS_STRING, NULL);
    enum_case(sl_align_ce, "Left", "left");
    enum_case(sl_align_ce, "Center", "center");
    enum_case(sl_align_ce, "Right", "right");

    memcpy(&sl_grad_handlers, zend_get_std_object_handlers(), sizeof sl_grad_handlers);
    sl_grad_handlers.offset = XtOffsetOf(sl_grad_t, std);
    sl_grad_handlers.clone_obj = NULL;   /* immutable: share it */
    memcpy(&sl_shadow_handlers, zend_get_std_object_handlers(), sizeof sl_shadow_handlers);
    sl_shadow_handlers.offset = XtOffsetOf(sl_shadow_t, std);
    sl_shadow_handlers.clone_obj = NULL;

    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Gradient", NULL);
    sl_gradient_ce = zend_register_internal_class(&ce);
    sl_gradient_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | ZEND_ACC_NOT_SERIALIZABLE;
    sl_gradient_ce->create_object = grad_create;
    declare(sl_gradient_ce, "stops", (zend_type)ZEND_TYPE_INIT_CODE(IS_ARRAY, 0, 0));

    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "LinearGradient", linear_methods);
    sl_linear_ce = zend_register_internal_class_ex(&ce, sl_gradient_ce);
    sl_linear_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    declare(sl_linear_ce, "angle", (zend_type)ZEND_TYPE_INIT_CODE(IS_DOUBLE, 0, 0));

    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "RadialGradient", radial_methods);
    sl_radial_ce = zend_register_internal_class_ex(&ce, sl_gradient_ce);
    sl_radial_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    declare(sl_radial_ce, "cx", (zend_type)ZEND_TYPE_INIT_CODE(IS_DOUBLE, 0, 0));
    declare(sl_radial_ce, "cy", (zend_type)ZEND_TYPE_INIT_CODE(IS_DOUBLE, 0, 0));
    declare(sl_radial_ce, "r", (zend_type)ZEND_TYPE_INIT_CODE(IS_DOUBLE, 0, 0));

    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Shadow", shadow_methods);
    sl_shadow_ce = zend_register_internal_class(&ce);
    sl_shadow_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    sl_shadow_ce->create_object = shadow_create;
    declare(sl_shadow_ce, "dx", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));
    declare(sl_shadow_ce, "dy", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));
    declare(sl_shadow_ce, "blur", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));
    declare(sl_shadow_ce, "color", (zend_type)ZEND_TYPE_INIT_MASK(MAY_BE_LONG | MAY_BE_DOUBLE));
}
