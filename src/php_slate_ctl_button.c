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
 * Button (a Control, php_slate_control.c). Its parts, bottom to top:
 *   - bg: a ROUNDRECT 0..w x h, radius h / 2 (Shape::Pill) or the theme's (Rounded), the variant's fill and border;
 *     its FILL eases in SLATE_CTL_PRESS_MS (OutCubic), so the pressed look slides (12 % toward white; for Ghost and
 *     Outline, which have no fill: the text colour at 6 %, the same RGB at alpha 0 at rest). A change of $variant or
 *     $fill jumps (EASE off, SET, EASE on again);
 *   - icon (with $icon only): an IMAGE of the store at (x, (h - 20) / 2), tinted with the text colour (always at alpha
 *     255: a tint never goes to or from none, which would show or hide the icon at once), so an icon is monochrome;
 *   - label: a TEXT in the theme's font, at (h - line height) / 2; centred with the icon in the width (with w = 0 the
 *     width is the content plus the paddings, so it starts at the padding). No text and an icon: the icon centred.
 * The size: h from the theme ($size), w given or from the content (slate_ctl_button_w()). A write sends only the SETs
 * whose value changed (the values sent are kept in the state).
 */
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"

#include "controls.h"
#include "images.h"
#include "php_slate.h"
#include "raster.h"

#if SLATE_HAS_CONTROLS

static zend_class_entry *sl_button_ce;

enum { V_PRIMARY, V_SECONDARY, V_GHOST, V_DANGER, V_OUTLINE };
static const char *const variant_names[] = { "Primary", "Secondary", "Ghost", "Danger", "Outline" };
static const char *const shape_names[] = { "Pill", "Rounded" };

static uint32_t s_off_text, s_off_variant, s_off_fill, s_off_color;
static zend_string *s_x_name, *s_y_name;

/* What was sent to the parts (or would be, before the control is realized). */
typedef struct {
    float w, h, radius, border, lx, ly, ix, iy;
    uint32_t fill, bcolor, tcolor;
} button_sent_t;

typedef struct {
    uint8_t size;      /* slate_ctl_size_t */
    uint8_t shape;     /* 0 Pill, 1 Rounded */
    bool down;         /* the pressed look */
    int wgiven;        /* 0: from the content */
    slate_image_t *icon;
    int bg, ic, lb;    /* part indexes (ic -1: no icon) */
    button_sent_t s;
} button_t;

static button_t *st_of(sl_obj_t *o)
{
    return o->ctl ? (button_t *)o->ctl->st : NULL;
}

/* The fill at rest and pressed, the border, the text colour of the variant (and of $fill, $color). */
static void colours(sl_obj_t *o, const sl_theme_t *th, uint32_t *rest, uint32_t *pressed, float *border,
                    uint32_t *bcolor, uint32_t *text)
{
    int v = sl_enum_case(OBJ_PROP(&o->std, s_off_variant), variant_names, 5);
    zval *fill = OBJ_PROP(&o->std, s_off_fill), *color = OBJ_PROP(&o->std, s_off_color);
    const uint32_t *c = th->c;
    uint32_t solid = v == V_PRIMARY ? c[SL_T_ACCENT] : v == V_SECONDARY ? c[SL_T_RAISED] : c[SL_T_DANGER];
    bool has_fill = v == V_PRIMARY || v == V_SECONDARY || v == V_DANGER;
    if (Z_TYPE_P(fill) == IS_LONG) {
        solid = (uint32_t)Z_LVAL_P(fill) & 0xFFFFFFu;
        has_fill = true;
    }
    /* no fill: text at alpha 0, the same RGB as pressed (text at 6 %): only the alpha slides */
    *rest = has_fill ? sl_rgb(solid) : slate_ctl_alpha(c[SL_T_TEXT], 0);
    *pressed = slate_ctl_pressed(*rest, c[SL_T_TEXT]);
    *border = v == V_SECONDARY || v == V_OUTLINE ? 1.0f : 0.0f;
    *bcolor = v == V_SECONDARY  ? sl_rgb(c[SL_T_BORDER_STRONG])
              : v == V_OUTLINE ? slate_ctl_alpha(c[SL_T_ACCENT], 60)
                               : 0;
    uint32_t t = v == V_SECONDARY ? c[SL_T_TEXT] : v == V_GHOST ? c[SL_T_TEXT_MUTED] : v == V_OUTLINE ? c[SL_T_ACCENT]
                                                                                                    : c[SL_T_ON_ACCENT];
    if (Z_TYPE_P(color) == IS_LONG) {
        t = (uint32_t)Z_LVAL_P(color) & 0xFFFFFFu;
    }
    *text = sl_rgb(t);
}

static bool set_num(sl_obj_t *o, int part, slate_prop_t prop, float *sent, float v, bool all)
{
    if (!all && *sent == v) {
        return true;
    }
    *sent = v;
    return sl_part_set(o, part, prop, slate_v_num(v));
}

static bool set_color(sl_obj_t *o, int part, slate_prop_t prop, uint32_t *sent, uint32_t v, bool all)
{
    if (!all && *sent == v) {
        return true;
    }
    *sent = v;
    return sl_part_set(o, part, prop, slate_v_color(v));
}

/* Lays the button out and sends what changed (all: everything, the realization). instant: a change of the fill
   jumps (a write of $variant or $fill), else it eases (the pressed look). text: the text changed. false with an
   exception. */
static bool apply(sl_obj_t *o, bool all, bool instant, bool text)
{
    button_t *b = st_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!b || !th) {
        return true;
    }
    zval *tz = OBJ_PROP(&o->std, s_off_text);
    const char *str = Z_TYPE_P(tz) == IS_STRING ? Z_STRVAL_P(tz) : "";
    bool has_text = str[0] != '\0';
    int tw = 0, lh = 0;
    if (th->font) {
        slate_rect_t r = slate_text_bbox(0, 0, str, th->font, SLATE_ALIGN_LEFT, 0);
        tw = r.x1 - r.x0;
        lh = slate_text_bbox(0, 0, " ", th->font, SLATE_ALIGN_LEFT, 0).y1;
    }
    bool icon = b->ic >= 0;
    int h = th->size_h[b->size];
    int w = b->wgiven ? b->wgiven : slate_ctl_button_w(has_text ? tw : 0, (slate_ctl_size_t)b->size, icon, h);
    if (w > 32767) {
        w = 32767;
    }
    int lead = icon ? SLATE_CTL_ICON + SLATE_CTL_ICON_GAP : 0;
    float lx = (float)((w - tw - lead) / 2 + lead), ly = (float)((h - lh) / 2);
    float ix = (float)(has_text ? (w - tw - lead) / 2 : (w - SLATE_CTL_ICON) / 2);
    float iy = (float)((h - SLATE_CTL_ICON) / 2);
    uint32_t rest, pressed, bcolor, tcolor;
    float border;
    colours(o, th, &rest, &pressed, &border, &bcolor, &tcolor);
    uint32_t fill = b->down ? pressed : rest;

    /* the group's $w and $h: the size laid out */
    o->ctl->w = (int16_t)w;
    o->ctl->h = (int16_t)h;
    zval *zw = sl_slot(o, "w"), *zh = sl_slot(o, "h");
    bool resized = !zw || Z_TYPE_P(zw) != IS_LONG || Z_LVAL_P(zw) != w || !zh || Z_TYPE_P(zh) != IS_LONG
                   || Z_LVAL_P(zh) != h;
    if (zw && zh && resized) {
        zval_ptr_dtor(zw);
        ZVAL_LONG(zw, w);
        zval_ptr_dtor(zh);
        ZVAL_LONG(zh, h);
        if (!all && sl_realized(o)) {
            slate_cmd_t cw = { .kind = SLATE_CMD_SET, .id = o->id, .prop = SLATE_PROP_W, .v = slate_v_num((float)w) };
            slate_cmd_t ch = { .kind = SLATE_CMD_SET, .id = o->id, .prop = SLATE_PROP_H, .v = slate_v_num((float)h) };
            if (!sl_push(&cw) || !sl_push(&ch)) {
                return false;
            }
        }
    }

    button_sent_t *s = &b->s;
    float radius = b->shape == 0 ? (float)h / 2 : (float)th->radius;
    if (!set_num(o, b->bg, SLATE_PROP_W, &s->w, (float)w, all) || !set_num(o, b->bg, SLATE_PROP_H, &s->h, (float)h, all)
        || !set_num(o, b->bg, SLATE_PROP_RADIUS, &s->radius, radius, all)
        || !set_num(o, b->bg, SLATE_PROP_BORDER, &s->border, border, all)
        || !set_color(o, b->bg, SLATE_PROP_BORDER_COLOR, &s->bcolor, bcolor, all)) {
        return false;
    }
    if (all || s->fill != fill) {
        bool jump = instant && !all && sl_realized(o);
        if ((jump && !sl_part_ease(o, b->bg, 0, 0, SLATE_EASE_LINEAR))
            || !set_color(o, b->bg, SLATE_PROP_FILL, &s->fill, fill, true)
            || (jump && !sl_part_ease(o, b->bg, 1u << SLATE_PROP_FILL, SLATE_CTL_PRESS_MS, SLATE_EASE_OUT_CUBIC))) {
            return false;
        }
    }
    if (all && !sl_part_ease(o, b->bg, 1u << SLATE_PROP_FILL, SLATE_CTL_PRESS_MS, SLATE_EASE_OUT_CUBIC)) {
        return false;   /* after the first fill: it is there at once, later ones slide */
    }
    if (icon) {
        if ((all && !sl_part_set(o, b->ic, SLATE_PROP_IMAGE, slate_v_image(b->icon)))
            || !set_num(o, b->ic, SLATE_PROP_X, &s->ix, ix, all) || !set_num(o, b->ic, SLATE_PROP_Y, &s->iy, iy, all)) {
            return false;
        }
    }
    if ((all || text) && !sl_part_set(o, b->lb, SLATE_PROP_TEXT, slate_v_text(str))) {
        return false;
    }
    if (all && !sl_part_set(o, b->lb, SLATE_PROP_FONT, slate_v_font(th->font))) {
        return false;
    }
    if (!set_num(o, b->lb, SLATE_PROP_X, &s->lx, lx, all) || !set_num(o, b->lb, SLATE_PROP_Y, &s->ly, ly, all)) {
        return false;
    }
    if (all || s->tcolor != tcolor) {
        s->tcolor = tcolor;
        if (!sl_part_set(o, b->lb, SLATE_PROP_COLOR, slate_v_color(tcolor))
            || (icon && !sl_part_set(o, b->ic, SLATE_PROP_TINT, slate_v_color(tcolor)))) {
            return false;
        }
    }
    return true;
}

/* ---- ops ---- */

static bool button_realize(sl_obj_t *o)
{
    return apply(o, true, false, true);
}

static void button_pressed(sl_obj_t *o, bool down)
{
    button_t *b = st_of(o);
    if (b && b->down != down) {
        b->down = down;
        apply(o, false, false, false);   /* an exception (no memory for the change) is the caller's */
    }
}

static bool button_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    bool fill = strcmp(name, "fill") == 0;
    if ((fill || strcmp(name, "color") == 0) && Z_TYPE_P(slot) == IS_LONG
        && (Z_LVAL_P(slot) < 0 || Z_LVAL_P(slot) > 0xFFFFFF)) {
        zend_value_error("%s::$%s must be a color, 0xRRGGBB", ZSTR_VAL(o->std.ce->name), name);
        return false;
    }
    if (zend_is_identical(old, slot)) {
        return true;
    }
    bool text = strcmp(name, "text") == 0;
    return apply(o, false, fill || strcmp(name, "variant") == 0, text);
}

bool sl_button_width(sl_obj_t *o, int w)
{
    button_t *b = o->base == SL_BUTTON ? st_of(o) : NULL;
    if (!b) {
        return true;
    }
    w = w < 1 ? 1 : w > 32767 ? 32767 : w;
    if (b->wgiven == w) {
        return true;
    }
    b->wgiven = w;
    return apply(o, false, false, false);
}

static void button_free(sl_obj_t *o)
{
    button_t *b = st_of(o);
    slate_image_put(b->icon);
    efree(b);
}

static const sl_ctl_ops_t button_ops = {
    .realize = button_realize,
    .pressed = button_pressed,
    .write = button_write,
    .free = button_free,
};

/* ---- Button ---- */

PHP_METHOD(SlateButton, __construct)
{
    zval *x, *y;
    zend_string *text, *icon = NULL;
    zend_object *variant = NULL, *size = NULL, *shape = NULL;
    zend_long w = 0;
    ZEND_PARSE_PARAMETERS_START(3, 8)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_STR(text)
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJ_OF_CLASS(variant, sl_variant_ce)
        Z_PARAM_OBJ_OF_CLASS(size, sl_size_ce)
        Z_PARAM_LONG(w)
        Z_PARAM_OBJ_OF_CLASS(shape, sl_shape_ce)
        Z_PARAM_STR_OR_NULL(icon)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    sl_obj_t *o = sl_obj(obj);
    const char *cls = ZSTR_VAL(obj->ce->name);
    if (!sl_ctl_need_slate(o)) {
        RETURN_THROWS();
    }
    if (!o->ctl || o->ctl->st) {
        zend_throw_error(NULL, "%s is already constructed", cls);
        RETURN_THROWS();
    }
    if (w < 0 || w > 32767) {
        zend_value_error("%s::$w must be between 0 and 32767", cls);
        RETURN_THROWS();
    }
    if (icon && !sl_image_icon_ok(cls, "icon", icon)) {
        RETURN_THROWS();
    }
    obj->handlers->write_property(obj, s_x_name, x, NULL);
    if (!EG(exception)) {
        obj->handlers->write_property(obj, s_y_name, y, NULL);
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
    zval *slot = OBJ_PROP(obj, s_off_text);
    zval_ptr_dtor(slot);
    ZVAL_STR_COPY(slot, text);
    slot = OBJ_PROP(obj, s_off_variant);
    zval_ptr_dtor(slot);
    ZVAL_OBJ_COPY(slot, variant ? variant : zend_enum_get_case_cstr(sl_variant_ce, "Secondary"));
    slate_image_t *im = icon ? sl_image_acquire(icon) : NULL;
    if (icon && !im) {
        RETURN_THROWS();
    }
    button_t *b = ecalloc(1, sizeof *b);
    zval zv;
    ZVAL_OBJ(&zv, size ? size : zend_enum_get_case_cstr(sl_size_ce, "Medium"));
    b->size = (uint8_t)sl_size_of(&zv);
    ZVAL_OBJ(&zv, shape ? shape : zend_enum_get_case_cstr(sl_shape_ce, "Rounded"));
    b->shape = (uint8_t)sl_enum_case(&zv, shape_names, 2);
    b->wgiven = (int)w;
    b->icon = im;
    b->ic = -1;
    o->ctl->st = b;
    b->bg = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    if (im) {
        b->ic = sl_part_add(o, SLATE_EL_IMAGE, -1);
    }
    b->lb = sl_part_add(o, SLATE_EL_TEXT, -1);
    if (EG(exception) || !apply(o, false, false, false)) {   /* the layout (not realized: nothing is sent) */
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_button_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, x, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_MASK(0, y, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, variant, Baremetal\\Slate\\Variant, 0,
                                         "\\Baremetal\\Slate\\Variant::Secondary")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, size, Baremetal\\Slate\\Size, 0, "\\Baremetal\\Slate\\Size::Medium")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, w, IS_LONG, 0, "0")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, shape, Baremetal\\Slate\\Shape, 0, "\\Baremetal\\Slate\\Shape::Rounded")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, icon, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_button_pressed, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, down, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry button_methods[] = {
    PHP_ME(SlateButton, __construct, arginfo_button_ctor, ZEND_ACC_PUBLIC)
    ZEND_MALIAS(SlateControl, pressed, builtinPressed, arginfo_button_pressed, ZEND_ACC_PROTECTED)
    PHP_FE_END
};

static uint32_t declare(zend_class_entry *ce, const char *name, zend_type t, bool null_default)
{
    zval dv;
    if (null_default) {
        ZVAL_NULL(&dv);
    } else {
        ZVAL_UNDEF(&dv);
    }
    zend_property_info *pi = zend_declare_typed_property(ce, zend_string_init_interned(name, strlen(name), 1), &dv,
                                                         ZEND_ACC_PUBLIC, NULL, t);
    return pi->offset;
}

void sl_button_register(void)
{
    s_x_name = zend_string_init_interned("x", 1, 1);
    s_y_name = zend_string_init_interned("y", 1, 1);
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Button", button_methods);
    sl_button_ce = zend_register_internal_class_ex(&ce, sl_control_ce);   /* not final: an app may extend it */
    sl_button_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    static const char variant_cls[] = "Baremetal\\Slate\\Variant";
    zend_string *variant = zend_string_init_interned(variant_cls, sizeof variant_cls - 1, 1);
    s_off_text = declare(sl_button_ce, "text", (zend_type)ZEND_TYPE_INIT_CODE(IS_STRING, 0, 0), false);
    s_off_variant = declare(sl_button_ce, "variant", (zend_type)ZEND_TYPE_INIT_CLASS(variant, 0, 0), false);
    s_off_fill = declare(sl_button_ce, "fill", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 1, 0), true);
    s_off_color = declare(sl_button_ce, "color", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 1, 0), true);
    sl_ctl_class(sl_button_ce, SL_BUTTON, &button_ops);
}

#endif
