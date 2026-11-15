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
 * The display controls (Controls, php_slate_control.c): ProgressRing, ProgressBar, Spinner, StepList. None of them
 * takes touches by default ($touchable false: each class redeclares Element's); a write from the code animates them,
 * none notifies.
 *
 * ProgressRing: $x, $y are its centre (cx, cy), its size 2r + width square (sl_obj_box centres it; no group W / H
 * is sent, so $clip has no effect). Parts:
 *   - track: an ARC 0..360 of radius r and width, well;
 *   - arc: an ARC from -90 to slate_ctl_ring_to($value), round caps, accent or $color; EASE {TO} in
 *     SLATE_CTL_PROGRESS_MS, so a write of $value slides;
 *   - num, pct (with $percent): TEXTs, round($value * 100) in fontDisplay and text, then "%" in font and accent,
 *     centred together (their widths from slate_text_bbox), their bottoms on one line; hidden while spinning.
 * $spinning: the arc jumps to -90..0 and an internal tween (no TweenDone) turns it for ever (from 270 to 360 in
 * SLATE_CTL_SPIN_MS, Linear, repeat -1); false: TWEEN_STOP and the arc back to -90..value at once (its ease off
 * for the jump).
 *
 * ProgressBar: $x, $y its top-left, w x h. Parts: track (a ROUNDRECT w x h, radius h / 2, well), fill (a ROUNDRECT
 * w * $value wide, the same radius, accent or $color; EASE {W} in SLATE_CTL_PROGRESS_MS). $glow: a shadow 0, 0 of
 * blur 10 px in the fill's colour on the fill; without shadows in the firmware nothing is sent.
 *
 * Spinner: $x, $y its centre, size 2r + 3 (no group W / H either). One part, arc (an ARC of radius r, width 3,
 * -90..0, round caps, accent or $color), turned by an internal tween started at the realization (repeat -1: it runs
 * only while the spinner is shown, the engine's rule for every tween).
 *
 * StepList: $x, $y its top-left, w x (56 n - 8). Six parts per step i (row slate_ctl_step_row(i, w)):
 *   - row: a ROUNDRECT w x 48, radius 14, no fill, border 1 in accent, at alpha 255 for the active step and 0 for the
 *     others (EASE {BORDER_COLOR});
 *   - dot: a CIRCLE of radius 12 at (24, 24), border 2: done, fill success at 15 %; to do, border borderStrong; active,
 *     neither (EASE {FILL, BORDER_COLOR}; the colours left out are the same hue at alpha 0, so they fade);
 *   - ck1, ck2: the LINEs of the check (slate_ctl_check), width 2, success, visible for a done step only;
 *   - spin: an ARC of radius 9, width 3, accent, round caps, visible for the active step only, turned by an internal
 *     tween (repeat -1) while it is active;
 *   - label: a TEXT in font at x 48, centred in the row: done textMuted, active text, to do textDim (EASE {COLOR}).
 * All eases take SLATE_CTL_PROGRESS_MS (OutCubic). A write of $active (0..n) sends the colours that change; the steps
 * that become done "pop": the dot's radius from 40 % to 108 % in SLATE_CTL_POP_MS1 (OutCubic), then to 100 % in
 * SLATE_CTL_POP_MS2 (Linear, chained), internal tweens without TweenDone. 21 steps at most (127 parts).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "php.h"
#include "zend_exceptions.h"

#include "controls.h"
#include "php_slate.h"
#include "raster.h"

#if SLATE_HAS_CONTROLS

static zend_class_entry *sl_ring_ce, *sl_bar_ce, *sl_spinner_ce, *sl_steps_ce;
static zend_string *s_x_name, *s_y_name;

/* ---- shared ---- */

/* The laid-out size: ctl->w / h and the $w / $h slots (the realization sends them as the group's W and H, except for
   the centred ProgressRing and Spinner: php_slate_elements.c, sl_realize). Fixed after the constructor. */
static void set_size(sl_obj_t *o, int w, int h)
{
    o->ctl->w = (int16_t)w;
    o->ctl->h = (int16_t)h;
    zval *zw = sl_slot(o, "w"), *zh = sl_slot(o, "h");
    if (zw && zh) {
        zval_ptr_dtor(zw);
        ZVAL_LONG(zw, w);
        zval_ptr_dtor(zh);
        ZVAL_LONG(zh, h);
    }
}

static bool check_color(sl_obj_t *o, const char *name, zval *slot)
{
    if (Z_TYPE_P(slot) == IS_LONG && (Z_LVAL_P(slot) < 0 || Z_LVAL_P(slot) > 0xFFFFFF)) {
        zend_value_error("%s::$%s must be a color, 0xRRGGBB", ZSTR_VAL(o->std.ce->name), name);
        return false;
    }
    return true;
}

/* $color of o (slot off) or the theme's accent, opaque. */
static uint32_t color_of(sl_obj_t *o, uint32_t off, const sl_theme_t *th)
{
    zval *z = OBJ_PROP(&o->std, off);
    return sl_rgb(Z_TYPE_P(z) == IS_LONG ? (uint32_t)Z_LVAL_P(z) : th->c[SL_T_ACCENT]);
}

/* A value of 0..1 from slot (a float): \ValueError when not finite, clamped otherwise (written back). */
static bool unit_value(sl_obj_t *o, zval *slot, double *v)
{
    double d = zval_get_double(slot);
    if (!isfinite(d)) {
        zend_value_error("%s::$value must be a finite number", ZSTR_VAL(o->std.ce->name));
        return false;
    }
    d = d < 0 ? 0 : d > 1 ? 1 : d;
    zval_ptr_dtor(slot);
    ZVAL_DOUBLE(slot, d);
    *v = d;
    return true;
}

static bool num(sl_obj_t *o, int part, slate_prop_t prop, float v)
{
    return sl_part_set(o, part, prop, slate_v_num(v));
}

static bool color(sl_obj_t *o, int part, slate_prop_t prop, uint32_t argb)
{
    return sl_part_set(o, part, prop, slate_v_color(argb));
}

/* The x and y of the control, written through its handler (checked as Element's). */
static bool place(zend_object *obj, zval *x, zval *y)
{
    obj->handlers->write_property(obj, s_x_name, x, NULL);
    if (!EG(exception)) {
        obj->handlers->write_property(obj, s_y_name, y, NULL);
    }
    return !EG(exception);
}

/* The turn of an arc part: from -90..0 to 270..360, SLATE_CTL_SPIN_MS, Linear, for ever. The part must be at
   -90..0 (no ease on FROM or TO). */
static bool spin(sl_obj_t *o, int part, uint32_t *id)
{
    slate_tween_prop_t props[2] = {
        { .prop = SLATE_PROP_FROM, .type = SLATE_VAL_NUM, .to.num = 270 },
        { .prop = SLATE_PROP_TO, .type = SLATE_VAL_NUM, .to.num = 360 },
    };
    return sl_part_tween(o, part, props, 2, SLATE_CTL_SPIN_MS, 0, SLATE_EASE_LINEAR, -1, 0, false, id);
}

/* The width and the line height of str in font (0, 0 without a font). */
static void measure(const char *str, const struct display_font_entry *font, int *w, int *lh)
{
    *w = *lh = 0;
    if (!font) {
        return;
    }
    if (str[0]) {
        slate_rect_t r = slate_text_bbox(0, 0, str, font, SLATE_ALIGN_LEFT, 0);
        *w = r.x1 - r.x0;
    }
    *lh = slate_text_bbox(0, 0, " ", font, SLATE_ALIGN_LEFT, 0).y1;
}

static uint32_t declare(zend_class_entry *ce, const char *name, zend_type t, zval *dv, uint32_t flags)
{
    zval undef;
    ZVAL_UNDEF(&undef);
    zend_property_info *pi = zend_declare_typed_property(ce, zend_string_init_interned(name, strlen(name), 1),
                                                         dv ? dv : &undef, ZEND_ACC_PUBLIC | flags, NULL, t);
    return pi->offset;
}

/* Element's $touchable redeclared false: a display control takes no touch by default. */
static void not_touchable(zend_class_entry *ce)
{
    zval dv;
    ZVAL_FALSE(&dv);
    declare(ce, "touchable", (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0), &dv, 0);
}

/* The common start of a constructor: a running Slate, not constructed yet. */
static bool ctor_start(sl_obj_t *o)
{
    if (!sl_ctl_need_slate(o)) {
        return false;
    }
    if (!o->ctl || o->ctl->st) {
        zend_throw_error(NULL, "%s is already constructed", ZSTR_VAL(o->std.ce->name));
        return false;
    }
    return true;
}

static bool in_range(sl_obj_t *o, const char *name, zend_long v, zend_long lo)
{
    if (v < lo || v > 32767) {
        zend_value_error("%s::$%s must be between " ZEND_LONG_FMT " and 32767", ZSTR_VAL(o->std.ce->name), name, lo);
        return false;
    }
    return true;
}

#define T_NUM (MAY_BE_LONG | MAY_BE_DOUBLE)

/* ================================ ProgressRing ================================ */

static uint32_t s_ring_value, s_ring_spinning, s_ring_color;

typedef struct {
    int r, width;
    bool percent;
    int track, arc, num, pct;   /* part indexes (num, pct: -1 without $percent) */
    uint32_t spin;              /* the turning tween (0: none) */
    struct {
        float to;
        uint32_t color;
        char text[8];
        float nx, px;
        bool texts;
    } s;                        /* what was sent */
} ring_t;

static ring_t *ring_of(sl_obj_t *o)
{
    return o->ctl ? (ring_t *)o->ctl->st : NULL;
}

static bool ring_spinning(sl_obj_t *o)
{
    return Z_TYPE_P(OBJ_PROP(&o->std, s_ring_spinning)) == IS_TRUE;
}

/* The percent texts: the number and its place (all: everything). */
static bool ring_texts(sl_obj_t *o, ring_t *rg, const sl_theme_t *th, bool all)
{
    if (!rg->percent) {
        return true;
    }
    double v = zval_get_double(OBJ_PROP(&o->std, s_ring_value));
    char text[8];
    snprintf(text, sizeof text, "%d", (int)lround(v * 100));
    int nw, nlh, pw, plh;
    measure(text, th->font_display, &nw, &nlh);
    measure("%", th->font, &pw, &plh);
    float nx = -(float)(nw + pw) / 2, px = nx + (float)nw, ny = -(float)nlh / 2;
    bool visible = !ring_spinning(o);
    if (all) {
        if (!sl_part_set(o, rg->num, SLATE_PROP_FONT, slate_v_font(th->font_display))
            || !color(o, rg->num, SLATE_PROP_COLOR, sl_rgb(th->c[SL_T_TEXT]))
            || !num(o, rg->num, SLATE_PROP_Y, ny)
            || !sl_part_set(o, rg->pct, SLATE_PROP_FONT, slate_v_font(th->font))
            || !sl_part_set(o, rg->pct, SLATE_PROP_TEXT, slate_v_text("%"))
            || !color(o, rg->pct, SLATE_PROP_COLOR, sl_rgb(th->c[SL_T_ACCENT]))
            || !num(o, rg->pct, SLATE_PROP_Y, ny + (float)(nlh - plh))) {
            return false;
        }
    }
    if (all || strcmp(text, rg->s.text) != 0) {
        memcpy(rg->s.text, text, sizeof text);
        if (!sl_part_set(o, rg->num, SLATE_PROP_TEXT, slate_v_text(text))) {
            return false;
        }
    }
    if (all || rg->s.nx != nx) {
        rg->s.nx = nx;
        if (!num(o, rg->num, SLATE_PROP_X, nx)) {
            return false;
        }
    }
    if (all || rg->s.px != px) {
        rg->s.px = px;
        if (!num(o, rg->pct, SLATE_PROP_X, px)) {
            return false;
        }
    }
    if (all || rg->s.texts != visible) {
        rg->s.texts = visible;
        if (!sl_part_set(o, rg->num, SLATE_PROP_VISIBLE, slate_v_bool(visible))
            || !sl_part_set(o, rg->pct, SLATE_PROP_VISIBLE, slate_v_bool(visible))) {
            return false;
        }
    }
    return true;
}

/* The arc at -90..0 at once (its ease off for the jump), then turning. */
static bool ring_spin_start(sl_obj_t *o, ring_t *rg, bool all)
{
    if (!all && !sl_part_ease(o, rg->arc, 0, 0, SLATE_EASE_LINEAR)) {
        return false;
    }
    rg->s.to = 0;
    if (!num(o, rg->arc, SLATE_PROP_FROM, -90) || !num(o, rg->arc, SLATE_PROP_TO, 0)
        || !sl_part_ease(o, rg->arc, 1u << SLATE_PROP_TO, SLATE_CTL_PROGRESS_MS, SLATE_EASE_OUT_CUBIC)) {
        return false;
    }
    return spin(o, rg->arc, &rg->spin);
}

static bool ring_apply(sl_obj_t *o, bool all)
{
    ring_t *rg = ring_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!rg || !th) {
        return true;
    }
    uint32_t c = color_of(o, s_ring_color, th);
    float r = (float)rg->r, w = (float)rg->width;
    bool spinning = ring_spinning(o);
    if (all) {
        if (!num(o, rg->track, SLATE_PROP_R, r) || !num(o, rg->track, SLATE_PROP_WIDTH, w)
            || !num(o, rg->track, SLATE_PROP_FROM, 0) || !num(o, rg->track, SLATE_PROP_TO, 360)
            || !color(o, rg->track, SLATE_PROP_COLOR, sl_rgb(th->c[SL_T_WELL]))
            || !num(o, rg->arc, SLATE_PROP_R, r) || !num(o, rg->arc, SLATE_PROP_WIDTH, w)
            || !sl_part_set(o, rg->arc, SLATE_PROP_CAPS, slate_v_enum(SLATE_CAPS_ROUND))) {
            return false;
        }
    }
    if (all || rg->s.color != c) {
        rg->s.color = c;
        if (!color(o, rg->arc, SLATE_PROP_COLOR, c)) {
            return false;
        }
    }
    if (!ring_texts(o, rg, th, all)) {
        return false;
    }
    if (all) {
        if (spinning) {
            return ring_spin_start(o, rg, true);
        }
        rg->s.to = slate_ctl_ring_to((float)zval_get_double(OBJ_PROP(&o->std, s_ring_value)));
        return num(o, rg->arc, SLATE_PROP_FROM, -90) && num(o, rg->arc, SLATE_PROP_TO, rg->s.to)
               && sl_part_ease(o, rg->arc, 1u << SLATE_PROP_TO, SLATE_CTL_PROGRESS_MS, SLATE_EASE_OUT_CUBIC);
    }
    if (!spinning) {
        float to = slate_ctl_ring_to((float)zval_get_double(OBJ_PROP(&o->std, s_ring_value)));
        if (rg->s.to != to) {
            rg->s.to = to;
            return num(o, rg->arc, SLATE_PROP_TO, to);
        }
    }
    return true;
}

static bool ring_realize(sl_obj_t *o)
{
    ring_t *rg = ring_of(o);
    if (rg) {
        rg->spin = 0;   /* a tween of an earlier Slate is gone with it */
    }
    return ring_apply(o, true);
}

static bool ring_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    ring_t *rg = ring_of(o);
    if (strcmp(name, "color") == 0) {
        if (!check_color(o, name, slot)) {
            return false;
        }
    } else if (strcmp(name, "value") == 0) {
        double v;
        if (!unit_value(o, slot, &v)) {
            return false;
        }
    } else if (strcmp(name, "spinning") == 0 && rg && !zend_is_identical(old, slot)) {
        sl_anim_flush();
        if (Z_TYPE_P(slot) == IS_TRUE) {
            if (sl_realized(o) && !ring_spin_start(o, rg, false)) {
                return false;
            }
        } else {
            /* the arc back to the value at once (the ease off for the jump, as at the start of the turn) */
            uint32_t id = rg->spin;
            rg->spin = 0;
            float to = slate_ctl_ring_to((float)zval_get_double(OBJ_PROP(&o->std, s_ring_value)));
            rg->s.to = to;
            if (!sl_part_tween_stop(id, false)
                || (sl_realized(o)
                    && (!sl_part_ease(o, rg->arc, 0, 0, SLATE_EASE_LINEAR)
                        || !num(o, rg->arc, SLATE_PROP_FROM, -90) || !num(o, rg->arc, SLATE_PROP_TO, to)
                        || !sl_part_ease(o, rg->arc, 1u << SLATE_PROP_TO, SLATE_CTL_PROGRESS_MS,
                                         SLATE_EASE_OUT_CUBIC)))) {
                return false;
            }
        }
    }
    return ring_apply(o, false);
}

static const sl_ctl_ops_t ring_ops = {
    .realize = ring_realize,
    .write = ring_write,
};

PHP_METHOD(SlateProgressRing, __construct)
{
    zval *x, *y;
    zend_long r, width = 12;
    double value = 0;
    bool percent = false;
    ZEND_PARSE_PARAMETERS_START(3, 6)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_LONG(r)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(value)
        Z_PARAM_LONG(width)
        Z_PARAM_BOOL(percent)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    sl_obj_t *o = sl_obj(obj);
    if (!ctor_start(o) || !in_range(o, "r", r, 1) || !in_range(o, "width", width, 1)) {
        RETURN_THROWS();
    }
    zval *slot = OBJ_PROP(obj, s_ring_value);
    zval_ptr_dtor(slot);
    ZVAL_DOUBLE(slot, value);
    if (!unit_value(o, slot, &value) || !place(obj, x, y)) {
        RETURN_THROWS();
    }
    ring_t *rg = ecalloc(1, sizeof *rg);
    rg->r = (int)r;
    rg->width = (int)width;
    rg->percent = percent;
    rg->num = rg->pct = -1;
    o->ctl->st = rg;
    rg->track = sl_part_add(o, SLATE_EL_ARC, -1);
    rg->arc = sl_part_add(o, SLATE_EL_ARC, -1);
    if (percent) {
        rg->num = sl_part_add(o, SLATE_EL_TEXT, -1);
        rg->pct = sl_part_add(o, SLATE_EL_TEXT, -1);
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
    int side = 2 * rg->r + rg->width;
    set_size(o, side, side);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_ring_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, cx, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, cy, T_NUM, NULL)
    ZEND_ARG_TYPE_INFO(0, r, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, value, IS_DOUBLE, 0, "0.0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, width, IS_LONG, 0, "12")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, percent, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

static const zend_function_entry ring_methods[] = {
    PHP_ME(SlateProgressRing, __construct, arginfo_ring_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ================================ ProgressBar ================================ */

static uint32_t s_bar_value, s_bar_glow, s_bar_color;

typedef struct {
    int w, h;
    int track, fill;
    struct {
        float fw;
        uint32_t color;
        bool glow;
    } s;   /* what was sent */
} bar_t;

static bar_t *bar_of(sl_obj_t *o)
{
    return o->ctl ? (bar_t *)o->ctl->st : NULL;
}

static bool bar_apply(sl_obj_t *o, bool all)
{
    bar_t *b = bar_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!b || !th) {
        return true;
    }
    uint32_t c = color_of(o, s_bar_color, th);
    float h = (float)b->h, rad = h / 2;
    float fw = (float)(b->w * zval_get_double(OBJ_PROP(&o->std, s_bar_value)));
    bool glow = Z_TYPE_P(OBJ_PROP(&o->std, s_bar_glow)) == IS_TRUE;
    if (all) {
        if (!num(o, b->track, SLATE_PROP_W, (float)b->w) || !num(o, b->track, SLATE_PROP_H, h)
            || !num(o, b->track, SLATE_PROP_RADIUS, rad)
            || !color(o, b->track, SLATE_PROP_FILL, sl_rgb(th->c[SL_T_WELL]))
            || !num(o, b->fill, SLATE_PROP_H, h) || !num(o, b->fill, SLATE_PROP_RADIUS, rad)) {
            return false;
        }
    }
    if (all || b->s.fw != fw) {
        b->s.fw = fw;
        if (!num(o, b->fill, SLATE_PROP_W, fw)) {
            return false;
        }
    }
    bool cnew = all || b->s.color != c;
    if (cnew) {
        b->s.color = c;
        if (!color(o, b->fill, SLATE_PROP_FILL, c)) {
            return false;
        }
    }
#if SLATE_HAS_SHADOWS
    if ((all && glow) || (!all && (b->s.glow != glow || (glow && cnew)))) {
        slate_value_t sh = glow ? slate_v_shadow(0, 0, sl_blur_radius(10), c) : slate_v_shadow(0, 0, 0, 0);
        if (!sl_part_set(o, b->fill, SLATE_PROP_SHADOW, sh)) {
            return false;
        }
    }
#endif
    b->s.glow = glow;
    if (all && !sl_part_ease(o, b->fill, 1u << SLATE_PROP_W, SLATE_CTL_PROGRESS_MS, SLATE_EASE_OUT_CUBIC)) {
        return false;
    }
    return true;
}

static bool bar_realize(sl_obj_t *o)
{
    return bar_apply(o, true);
}

static bool bar_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)old;
    if (strcmp(name, "color") == 0) {
        if (!check_color(o, name, slot)) {
            return false;
        }
    } else if (strcmp(name, "value") == 0) {
        double v;
        if (!unit_value(o, slot, &v)) {
            return false;
        }
    }
    return bar_apply(o, false);
}

static const sl_ctl_ops_t bar_ops = {
    .realize = bar_realize,
    .write = bar_write,
};

PHP_METHOD(SlateProgressBar, __construct)
{
    zval *x, *y;
    zend_long w, h = 4;
    double value = 0;
    ZEND_PARSE_PARAMETERS_START(3, 5)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_LONG(w)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(value)
        Z_PARAM_LONG(h)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    sl_obj_t *o = sl_obj(obj);
    if (!ctor_start(o) || !in_range(o, "w", w, 1) || !in_range(o, "h", h, 1)) {
        RETURN_THROWS();
    }
    zval *slot = OBJ_PROP(obj, s_bar_value);
    zval_ptr_dtor(slot);
    ZVAL_DOUBLE(slot, value);
    if (!unit_value(o, slot, &value) || !place(obj, x, y)) {
        RETURN_THROWS();
    }
    bar_t *b = ecalloc(1, sizeof *b);
    b->w = (int)w;
    b->h = (int)h;
    o->ctl->st = b;
    b->track = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    b->fill = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    set_size(o, b->w, b->h);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_bar_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, NULL)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, value, IS_DOUBLE, 0, "0.0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, h, IS_LONG, 0, "4")
ZEND_END_ARG_INFO()

static const zend_function_entry bar_methods[] = {
    PHP_ME(SlateProgressBar, __construct, arginfo_bar_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ================================ Spinner ================================ */

static uint32_t s_spinner_color;

typedef struct {
    int r;
    int arc;
    uint32_t spin, color;   /* the turning tween; the colour sent */
} spinner_t;

static spinner_t *spinner_of(sl_obj_t *o)
{
    return o->ctl ? (spinner_t *)o->ctl->st : NULL;
}

static bool spinner_apply(sl_obj_t *o, bool all)
{
    spinner_t *s = spinner_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!s || !th) {
        return true;
    }
    uint32_t c = color_of(o, s_spinner_color, th);
    if (all || s->color != c) {
        s->color = c;
        if (!color(o, s->arc, SLATE_PROP_COLOR, c)) {
            return false;
        }
    }
    if (!all) {
        return true;
    }
    s->spin = 0;
    return num(o, s->arc, SLATE_PROP_R, (float)s->r) && num(o, s->arc, SLATE_PROP_WIDTH, SLATE_CTL_SPINNER_WIDTH)
           && sl_part_set(o, s->arc, SLATE_PROP_CAPS, slate_v_enum(SLATE_CAPS_ROUND))
           && num(o, s->arc, SLATE_PROP_FROM, -90) && num(o, s->arc, SLATE_PROP_TO, 0) && spin(o, s->arc, &s->spin);
}

static bool spinner_realize(sl_obj_t *o)
{
    return spinner_apply(o, true);
}

static bool spinner_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)old;
    if (strcmp(name, "color") == 0 && !check_color(o, name, slot)) {
        return false;
    }
    return spinner_apply(o, false);
}

static const sl_ctl_ops_t spinner_ops = {
    .realize = spinner_realize,
    .write = spinner_write,
};

PHP_METHOD(SlateSpinner, __construct)
{
    zval *x, *y;
    zend_long r = 12;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(r)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    sl_obj_t *o = sl_obj(obj);
    if (!ctor_start(o) || !in_range(o, "r", r, 1) || !place(obj, x, y)) {
        RETURN_THROWS();
    }
    spinner_t *s = ecalloc(1, sizeof *s);
    s->r = (int)r;
    o->ctl->st = s;
    s->arc = sl_part_add(o, SLATE_EL_ARC, -1);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    int side = 2 * s->r + SLATE_CTL_SPINNER_WIDTH;
    set_size(o, side, side);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_spinner_ctor, 0, 0, 2)
    ZEND_ARG_TYPE_MASK(0, cx, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, cy, T_NUM, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, r, IS_LONG, 0, "12")
ZEND_END_ARG_INFO()

static const zend_function_entry spinner_methods[] = {
    PHP_ME(SlateSpinner, __construct, arginfo_spinner_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ================================ StepList ================================ */

#define STEP_PARTS 6
#define STEPS_MAX (127 / STEP_PARTS)

enum { P_ROW, P_DOT, P_CK1, P_CK2, P_SPIN, P_LABEL };
enum { ST_DONE, ST_ACTIVE, ST_TODO };

static uint32_t s_steps_active, s_steps_steps;

typedef struct {
    int w, n;
    int part0;          /* the index of the first part (row 0's) */
    int active;         /* the $active the parts show */
    uint32_t spin;      /* the active step's turning tween */
    uint32_t *pop;      /* 2 per step: its pop's tweens (emalloc) */
} steps_t;

static steps_t *steps_of(sl_obj_t *o)
{
    return o->ctl ? (steps_t *)o->ctl->st : NULL;
}

static int step_state(int i, int active)
{
    return i < active ? ST_DONE : i == active ? ST_ACTIVE : ST_TODO;
}

static int part(const steps_t *s, int i, int k)
{
    return s->part0 + i * STEP_PARTS + k;
}

/* The look of step i in state st (colours eased, visibilities). */
static bool step_look(sl_obj_t *o, steps_t *s, const sl_theme_t *th, int i, int st)
{
    const uint32_t *t = th->c;
    uint32_t border = slate_ctl_alpha(t[SL_T_ACCENT], st == ST_ACTIVE ? 100 : 0);
    uint32_t fill = slate_ctl_alpha(t[SL_T_SUCCESS], st == ST_DONE ? 15 : 0);
    uint32_t ring = slate_ctl_alpha(t[SL_T_BORDER_STRONG], st == ST_TODO ? 100 : 0);
    uint32_t text = sl_rgb(t[st == ST_DONE ? SL_T_TEXT_MUTED : st == ST_ACTIVE ? SL_T_TEXT : SL_T_TEXT_DIM]);
    return color(o, part(s, i, P_ROW), SLATE_PROP_BORDER_COLOR, border)
           && color(o, part(s, i, P_DOT), SLATE_PROP_FILL, fill)
           && color(o, part(s, i, P_DOT), SLATE_PROP_BORDER_COLOR, ring)
           && sl_part_set(o, part(s, i, P_CK1), SLATE_PROP_VISIBLE, slate_v_bool(st == ST_DONE))
           && sl_part_set(o, part(s, i, P_CK2), SLATE_PROP_VISIBLE, slate_v_bool(st == ST_DONE))
           && sl_part_set(o, part(s, i, P_SPIN), SLATE_PROP_VISIBLE, slate_v_bool(st == ST_ACTIVE))
           && color(o, part(s, i, P_LABEL), SLATE_PROP_COLOR, text);
}

/* The spinner of step i (the active one) back at -90..0 and turning. */
static bool step_spin(sl_obj_t *o, steps_t *s, int i)
{
    int p = part(s, i, P_SPIN);
    return num(o, p, SLATE_PROP_FROM, -90) && num(o, p, SLATE_PROP_TO, 0) && spin(o, p, &s->spin);
}

static bool steps_realize(sl_obj_t *o)
{
    steps_t *s = steps_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!s || !th) {
        return true;
    }
    s->spin = 0;   /* the tweens of an earlier Slate are gone with it */
    memset(s->pop, 0, (size_t)s->n * 2 * sizeof *s->pop);
    s->active = (int)Z_LVAL_P(OBJ_PROP(&o->std, s_steps_active));
    const uint32_t *t = th->c;
    float ck[6];
    slate_ctl_check(ck);
    int lw, lh;
    measure("", th->font, &lw, &lh);
    zval *steps = OBJ_PROP(&o->std, s_steps_steps);
    for (int i = 0; i < s->n; i++) {
        slate_ctl_box_t b = slate_ctl_step_row(i, s->w);
        float y = (float)b.y, cy = y + SLATE_CTL_STEP_H / 2;
        zval *str = zend_hash_index_find(Z_ARRVAL_P(steps), (zend_ulong)i);
        int row = part(s, i, P_ROW), dot = part(s, i, P_DOT), ck1 = part(s, i, P_CK1), ck2 = part(s, i, P_CK2),
            sp = part(s, i, P_SPIN), label = part(s, i, P_LABEL);
        if (!num(o, row, SLATE_PROP_Y, y) || !num(o, row, SLATE_PROP_W, (float)b.w)
            || !num(o, row, SLATE_PROP_H, (float)b.h) || !num(o, row, SLATE_PROP_RADIUS, SLATE_CTL_STEP_RADIUS)
            || !color(o, row, SLATE_PROP_FILL, slate_ctl_alpha(t[SL_T_ACCENT], 0))
            || !num(o, row, SLATE_PROP_BORDER, 1)
            || !num(o, dot, SLATE_PROP_X, SLATE_CTL_STEP_DOT_X) || !num(o, dot, SLATE_PROP_Y, cy)
            || !num(o, dot, SLATE_PROP_R, SLATE_CTL_STEP_DOT) || !num(o, dot, SLATE_PROP_BORDER, 2)
            || !num(o, ck1, SLATE_PROP_X, ck[0]) || !num(o, ck1, SLATE_PROP_Y, y + ck[1])
            || !num(o, ck1, SLATE_PROP_X1, ck[2]) || !num(o, ck1, SLATE_PROP_Y1, y + ck[3])
            || !num(o, ck2, SLATE_PROP_X, ck[2]) || !num(o, ck2, SLATE_PROP_Y, y + ck[3])
            || !num(o, ck2, SLATE_PROP_X1, ck[4]) || !num(o, ck2, SLATE_PROP_Y1, y + ck[5])
            || !num(o, sp, SLATE_PROP_X, SLATE_CTL_STEP_DOT_X) || !num(o, sp, SLATE_PROP_Y, cy)
            || !num(o, sp, SLATE_PROP_R, SLATE_CTL_STEP_SPIN_R)
            || !num(o, sp, SLATE_PROP_WIDTH, SLATE_CTL_SPINNER_WIDTH)
            || !sl_part_set(o, sp, SLATE_PROP_CAPS, slate_v_enum(SLATE_CAPS_ROUND))
            || !color(o, sp, SLATE_PROP_COLOR, sl_rgb(t[SL_T_ACCENT]))
            || !num(o, sp, SLATE_PROP_FROM, -90) || !num(o, sp, SLATE_PROP_TO, 0)
            || !num(o, label, SLATE_PROP_X, SLATE_CTL_STEP_TEXT_X)
            || !num(o, label, SLATE_PROP_Y, y + (float)((SLATE_CTL_STEP_H - lh) / 2))
            || !sl_part_set(o, label, SLATE_PROP_FONT, slate_v_font(th->font))
            || !sl_part_set(o, label, SLATE_PROP_TEXT,
                            slate_v_text(str && Z_TYPE_P(str) == IS_STRING ? Z_STRVAL_P(str) : ""))) {
            return false;
        }
        for (int k = P_CK1; k <= P_CK2; k++) {
            int l = part(s, i, k);
            if (!num(o, l, SLATE_PROP_WIDTH, 2) || !sl_part_set(o, l, SLATE_PROP_CAPS, slate_v_enum(SLATE_CAPS_ROUND))
                || !color(o, l, SLATE_PROP_COLOR, sl_rgb(t[SL_T_SUCCESS]))) {
                return false;
            }
        }
        if (!step_look(o, s, th, i, step_state(i, s->active))
            || !sl_part_ease(o, row, 1u << SLATE_PROP_BORDER_COLOR, SLATE_CTL_PROGRESS_MS, SLATE_EASE_OUT_CUBIC)
            || !sl_part_ease(o, dot, (1u << SLATE_PROP_FILL) | (1u << SLATE_PROP_BORDER_COLOR), SLATE_CTL_PROGRESS_MS,
                             SLATE_EASE_OUT_CUBIC)
            || !sl_part_ease(o, label, 1u << SLATE_PROP_COLOR, SLATE_CTL_PROGRESS_MS, SLATE_EASE_OUT_CUBIC)) {
            return false;
        }
    }
    return s->active >= s->n || step_spin(o, s, s->active);
}

/* The pop of step i, just done: its dot's radius 40 % -> 108 % -> 100 %. */
static bool step_pop(sl_obj_t *o, steps_t *s, int i)
{
    int dot = part(s, i, P_DOT);
    const float r = SLATE_CTL_STEP_DOT;
    slate_tween_prop_t up = { .prop = SLATE_PROP_R, .type = SLATE_VAL_NUM, .to.num = r * 1.08f };
    slate_tween_prop_t back = { .prop = SLATE_PROP_R, .type = SLATE_VAL_NUM, .to.num = r };
    return num(o, dot, SLATE_PROP_R, r * 0.4f)
           && sl_part_tween(o, dot, &up, 1, SLATE_CTL_POP_MS1, 0, SLATE_EASE_OUT_CUBIC, 0, 0, false, &s->pop[2 * i])
           && sl_part_tween(o, dot, &back, 1, SLATE_CTL_POP_MS2, 0, SLATE_EASE_LINEAR, 0, s->pop[2 * i], false,
                            &s->pop[2 * i + 1]);
}

/* The parts from s->active to the $active written (realized only). */
static bool steps_move(sl_obj_t *o, steps_t *s, int to)
{
    sl_theme_t *th = sl_ctl_theme(o);
    int from = s->active;
    s->active = to;
    if (!th || from == to || !sl_realized(o)) {
        return true;
    }
    sl_anim_flush();
    if (from < s->n) {   /* the spinner of the step that was active stops */
        uint32_t id = s->spin;
        s->spin = 0;
        if (!sl_part_tween_stop(id, false)) {
            return false;
        }
    }
    for (int i = 0; i < s->n; i++) {
        int was = step_state(i, from), now = step_state(i, to);
        if (was == now) {
            continue;
        }
        if (!step_look(o, s, th, i, now)) {
            return false;
        }
        if (now == ST_DONE) {
            if (!step_pop(o, s, i)) {
                return false;
            }
        } else if (was == ST_DONE) {   /* done no more: its pop stops, the dot at its radius */
            uint32_t a = s->pop[2 * i], b = s->pop[2 * i + 1];
            s->pop[2 * i] = s->pop[2 * i + 1] = 0;
            if (!sl_part_tween_stop(a, false) || !sl_part_tween_stop(b, false)
                || !num(o, part(s, i, P_DOT), SLATE_PROP_R, SLATE_CTL_STEP_DOT)) {
                return false;
            }
        }
    }
    return to >= s->n || step_spin(o, s, to);
}

static bool steps_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)old;
    steps_t *s = steps_of(o);
    if (!s || strcmp(name, "active") != 0) {
        return true;
    }
    zend_long a = Z_LVAL_P(slot);
    if (a < 0 || a > s->n) {
        zend_value_error("%s::$active must be between 0 and %d", ZSTR_VAL(o->std.ce->name), s->n);
        return false;
    }
    return steps_move(o, s, (int)a);
}

static void steps_free(sl_obj_t *o)
{
    steps_t *s = steps_of(o);
    if (s) {
        efree(s->pop);
        efree(s);
    }
}

static const sl_ctl_ops_t steps_ops = {
    .realize = steps_realize,
    .write = steps_write,
    .free = steps_free,
};

PHP_METHOD(SlateStepList, __construct)
{
    zval *x, *y;
    zend_long w, active = 0;
    HashTable *steps;
    ZEND_PARSE_PARAMETERS_START(4, 5)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_LONG(w)
        Z_PARAM_ARRAY_HT(steps)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(active)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    sl_obj_t *o = sl_obj(obj);
    const char *cls = ZSTR_VAL(obj->ce->name);
    if (!ctor_start(o) || !in_range(o, "w", w, 1)) {
        RETURN_THROWS();
    }
    uint32_t n = zend_hash_num_elements(steps);
    bool ok = n > 0 && zend_array_is_list(steps);
    zval *zv;
    ZEND_HASH_FOREACH_VAL(steps, zv) {
        ok = ok && Z_TYPE_P(zv) == IS_STRING;
    } ZEND_HASH_FOREACH_END();
    if (!ok) {
        zend_value_error("%s::$steps must be a non-empty list of strings", cls);
        RETURN_THROWS();
    }
    if (n > STEPS_MAX) {
        zend_value_error("%s::$steps must hold at most %d steps", cls, STEPS_MAX);
        RETURN_THROWS();
    }
    if (active < 0 || active > (zend_long)n) {
        zend_value_error("%s::$active must be between 0 and %d", cls, (int)n);
        RETURN_THROWS();
    }
    if (!place(obj, x, y)) {
        RETURN_THROWS();
    }
    zval *slot = OBJ_PROP(obj, s_steps_steps);
    zval_ptr_dtor(slot);
    ZVAL_ARR(slot, zend_array_dup(steps));
    slot = OBJ_PROP(obj, s_steps_active);
    zval_ptr_dtor(slot);
    ZVAL_LONG(slot, active);
    steps_t *s = ecalloc(1, sizeof *s);
    s->w = (int)w;
    s->n = (int)n;
    s->active = (int)active;
    s->pop = ecalloc(n * 2, sizeof *s->pop);
    o->ctl->st = s;
    for (uint32_t i = 0; i < n; i++) {
        static const uint8_t kinds[STEP_PARTS] = { SLATE_EL_ROUNDRECT, SLATE_EL_CIRCLE, SLATE_EL_LINE, SLATE_EL_LINE,
                                                   SLATE_EL_ARC, SLATE_EL_TEXT };
        for (int k = 0; k < STEP_PARTS; k++) {
            int p = sl_part_add(o, (slate_el_kind_t)kinds[k], -1);
            if (p < 0) {
                RETURN_THROWS();
            }
            if (i == 0 && k == 0) {
                s->part0 = p;
            }
        }
    }
    slate_ctl_box_t last = slate_ctl_step_row((int)n - 1, s->w);
    set_size(o, s->w, last.y + last.h);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_steps_ctor, 0, 0, 4)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, NULL)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, steps, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, active, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

static const zend_function_entry steps_methods[] = {
    PHP_ME(SlateStepList, __construct, arginfo_steps_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ================================ registration ================================ */

static zend_class_entry *reg(const char *name, const zend_function_entry *methods)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), methods);
    zend_class_entry *c = zend_register_internal_class_ex(&ce, sl_control_ce);   /* not final: an app may extend it */
    c->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    not_touchable(c);
    return c;
}

void sl_progress_register(void)
{
    s_x_name = zend_string_init_interned("x", 1, 1);
    s_y_name = zend_string_init_interned("y", 1, 1);
    zval dv;
    zend_type dbl = (zend_type)ZEND_TYPE_INIT_CODE(IS_DOUBLE, 0, 0),
              bln = (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0),
              nint = (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 1, 0);

    sl_ring_ce = reg("Baremetal\\Slate\\ProgressRing", ring_methods);
    s_ring_value = declare(sl_ring_ce, "value", dbl, NULL, 0);
    ZVAL_FALSE(&dv);
    s_ring_spinning = declare(sl_ring_ce, "spinning", bln, &dv, 0);
    ZVAL_NULL(&dv);
    s_ring_color = declare(sl_ring_ce, "color", nint, &dv, 0);
    sl_ctl_class(sl_ring_ce, SL_PROGRESS_RING, &ring_ops);

    sl_bar_ce = reg("Baremetal\\Slate\\ProgressBar", bar_methods);
    s_bar_value = declare(sl_bar_ce, "value", dbl, NULL, 0);
    ZVAL_FALSE(&dv);
    s_bar_glow = declare(sl_bar_ce, "glow", bln, &dv, 0);
    ZVAL_NULL(&dv);
    s_bar_color = declare(sl_bar_ce, "color", nint, &dv, 0);
    sl_ctl_class(sl_bar_ce, SL_PROGRESS_BAR, &bar_ops);

    sl_spinner_ce = reg("Baremetal\\Slate\\Spinner", spinner_methods);
    ZVAL_NULL(&dv);
    s_spinner_color = declare(sl_spinner_ce, "color", nint, &dv, 0);
    sl_ctl_class(sl_spinner_ce, SL_SPINNER, &spinner_ops);

    sl_steps_ce = reg("Baremetal\\Slate\\StepList", steps_methods);
    s_steps_active = declare(sl_steps_ce, "active", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0), NULL, 0);
    s_steps_steps = declare(sl_steps_ce, "steps", (zend_type)ZEND_TYPE_INIT_CODE(IS_ARRAY, 0, 0), NULL,
                            ZEND_ACC_READONLY);
    sl_ctl_class(sl_steps_ce, SL_STEP_LIST, &steps_ops);
}

#endif
