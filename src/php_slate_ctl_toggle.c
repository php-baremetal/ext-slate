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
 * Toggle (an on/off switch) and Slider (Controls, php_slate_control.c).
 *
 * Toggle: its parts, bottom to top:
 *   - track: a ROUNDRECT w x h (slate_ctl_toggle_dims()), radius h / 2; on: accent, off: borderStrong; pressed: that
 *     colour 12 % toward white. Its FILL eases in SLATE_CTL_TOGGLE_MS (so the pressed look takes 250 ms too);
 *   - knob: a CIRCLE of radius knob / 2, centre (knob_x + knob / 2, h / 2); on: bg, off: textMuted. Its X and FILL
 *     ease in SLATE_CTL_TOGGLE_MS.
 * A Tap inverts $on and notifies onChange(bool); a write of $on from the code slides the same way, without a
 * notification. $hitPad is 8 by default (the class redeclares Element's).
 *
 * Slider: its parts, bottom to top (no EASE: it follows the finger):
 *   - area: a ROUNDRECT w x H at alpha 0 (H the theme's height of the size): the height of the touch area;
 *   - track: a ROUNDRECT w x 8 at y (H - 8) / 2, radius 4, well;
 *   - fill: a ROUNDRECT from 0 to the knob's centre, as the track, accent or $fill;
 *   - knob: a CIRCLE of radius knob / 2 at (centre, H / 2), text, a border of 5 in accent or $fill.
 * The touch: Press keeps the value of before the press (v0), moves the knob under the finger (snapped) and the touch
 * is undecided; Moves within SLATE_CTL_SLIDER_SLOP px follow it without a notification; the first Move past it that
 * still reaches the slider (php_slate_touch.c did not hand it to a Scroll) makes the touch the slider's (ctl->owned),
 * and from there a changed value is notified at most once per frame of the engine (stats().frames): a value held back
 * goes out at a later Move after a frame, or at the Release. Release: the value under the finger; a value not notified
 * yet goes out, then onCommit() and the callback of committed() (always). Cancel before any notification (the hand-off
 * to a Scroll, or $enabled false, a show()... early): back to v0 (or to a value the code wrote during the touch), no
 * notification; Cancel after the drag notified: the value stays, a held-back one goes out, then the commit.
 * $value is int when $min, $max and $step are all int, float otherwise; a write of any of them from the code brings
 * $value into the range and onto the step, without a notification.
 */
#include <math.h>
#include <string.h>

#include "php.h"
#include "zend_closures.h"
#include "zend_enum.h"
#include "zend_exceptions.h"

#include "controls.h"
#include "php_slate.h"

#if SLATE_HAS_CONTROLS

static zend_class_entry *sl_toggle_ce, *sl_slider_ce;
static zend_string *s_x_name, *s_y_name;

/* The laid-out size of a control: ctl->w / h and its $w / $h slots (and, realized and not at the realization, the
   group's W and H). false with an exception. */
static bool set_size(sl_obj_t *o, int w, int h, bool all)
{
    o->ctl->w = (int16_t)w;
    o->ctl->h = (int16_t)h;
    zval *zw = sl_slot(o, "w"), *zh = sl_slot(o, "h");
    if (!zw || !zh || (Z_TYPE_P(zw) == IS_LONG && Z_LVAL_P(zw) == w && Z_TYPE_P(zh) == IS_LONG && Z_LVAL_P(zh) == h)) {
        return true;
    }
    zval_ptr_dtor(zw);
    ZVAL_LONG(zw, w);
    zval_ptr_dtor(zh);
    ZVAL_LONG(zh, h);
    if (!all && sl_realized(o)) {
        slate_cmd_t cw = { .kind = SLATE_CMD_SET, .id = o->id, .prop = SLATE_PROP_W, .v = slate_v_num((float)w) };
        slate_cmd_t ch = { .kind = SLATE_CMD_SET, .id = o->id, .prop = SLATE_PROP_H, .v = slate_v_num((float)h) };
        return sl_push(&cw) && sl_push(&ch);
    }
    return true;
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

static bool check_color(sl_obj_t *o, const char *name, zval *slot)
{
    if (Z_TYPE_P(slot) == IS_LONG && (Z_LVAL_P(slot) < 0 || Z_LVAL_P(slot) > 0xFFFFFF)) {
        zend_value_error("%s::$%s must be a color, 0xRRGGBB", ZSTR_VAL(o->std.ce->name), name);
        return false;
    }
    return true;
}

/* The method lcname of o's class unless it is base's own (internal, empty): NULL then. */
static zend_function *user_method(sl_obj_t *o, zend_class_entry *base, const char *lcname, size_t len)
{
    zend_function *fn = zend_hash_str_find_ptr(&o->std.ce->function_table, lcname, len);
    if (!fn || (fn->type == ZEND_INTERNAL_FUNCTION && fn->common.scope == base)) {
        return NULL;
    }
    return fn;
}

/* ================================ Toggle ================================ */

static uint32_t s_off_on;

typedef struct {
    uint8_t size;   /* slate_ctl_size_t */
    bool down;      /* the pressed look */
    int track, knob;
    struct {
        float w, h, radius, kx, ky, kr;
        uint32_t tfill, kfill;
    } s;            /* what was sent */
} toggle_t;

static toggle_t *tg_of(sl_obj_t *o)
{
    return o->ctl ? (toggle_t *)o->ctl->st : NULL;
}

/* Lays the toggle out and sends what changed (all: everything, the realization); the EASEs after the first values
   (all), so the later ones slide. false with an exception. */
static bool toggle_apply(sl_obj_t *o, bool all)
{
    toggle_t *tg = tg_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!tg || !th) {
        return true;
    }
    slate_ctl_toggle_t d = slate_ctl_toggle_dims((slate_ctl_size_t)tg->size);
    bool on = Z_TYPE_P(OBJ_PROP(&o->std, s_off_on)) == IS_TRUE;
    const uint32_t *c = th->c;
    uint32_t track = on ? c[SL_T_ACCENT] : c[SL_T_BORDER_STRONG];
    if (tg->down) {
        track = slate_ctl_mix(track, 0xFFFFFF, SLATE_CTL_PRESS_PCT);
    }
    uint32_t knob = on ? c[SL_T_BG] : c[SL_T_TEXT_MUTED];
    if (!set_size(o, d.w, d.h, all)) {
        return false;
    }
    if (!set_num(o, tg->track, SLATE_PROP_W, &tg->s.w, (float)d.w, all)
        || !set_num(o, tg->track, SLATE_PROP_H, &tg->s.h, (float)d.h, all)
        || !set_num(o, tg->track, SLATE_PROP_RADIUS, &tg->s.radius, (float)d.h / 2, all)
        || !set_color(o, tg->track, SLATE_PROP_FILL, &tg->s.tfill, sl_rgb(track), all)
        || !set_num(o, tg->knob, SLATE_PROP_X, &tg->s.kx,
                    (float)slate_ctl_toggle_knob_x(&d, on) + (float)d.knob / 2, all)
        || !set_num(o, tg->knob, SLATE_PROP_Y, &tg->s.ky, (float)d.h / 2, all)
        || !set_num(o, tg->knob, SLATE_PROP_R, &tg->s.kr, (float)d.knob / 2, all)
        || !set_color(o, tg->knob, SLATE_PROP_FILL, &tg->s.kfill, sl_rgb(knob), all)) {
        return false;
    }
    if (all
        && (!sl_part_ease(o, tg->track, 1u << SLATE_PROP_FILL, SLATE_CTL_TOGGLE_MS, SLATE_EASE_OUT_CUBIC)
            || !sl_part_ease(o, tg->knob, (1u << SLATE_PROP_X) | (1u << SLATE_PROP_FILL), SLATE_CTL_TOGGLE_MS,
                             SLATE_EASE_OUT_CUBIC))) {
        return false;
    }
    return true;
}

static bool toggle_realize(sl_obj_t *o)
{
    return toggle_apply(o, true);
}

static void toggle_pressed(sl_obj_t *o, bool down)
{
    toggle_t *tg = tg_of(o);
    if (tg && tg->down != down) {
        tg->down = down;
        toggle_apply(o, false);   /* an exception (no memory for the change) is the caller's */
    }
}

static bool toggle_touch(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly)
{
    (void)p;
    (void)lx;
    (void)ly;
    if (kind != SL_TOUCH_TAP || !tg_of(o)) {
        return true;
    }
    sl_anim_flush();
    zval *slot = OBJ_PROP(&o->std, s_off_on);
    bool on = Z_TYPE_P(slot) != IS_TRUE;
    ZVAL_BOOL(slot, on);
    if (!toggle_apply(o, false)) {
        return false;
    }
    zval v;
    ZVAL_BOOL(&v, on);
    return sl_ctl_notify(o, &v);
}

static bool toggle_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)name;
    if (zend_is_identical(old, slot)) {
        return true;
    }
    return toggle_apply(o, false);
}

static const sl_ctl_ops_t toggle_ops = {
    .realize = toggle_realize,
    .touch = toggle_touch,
    .pressed = toggle_pressed,
    .write = toggle_write,
};

PHP_METHOD(SlateToggle, __construct)
{
    zval *x, *y;
    bool on = false;
    zend_object *size = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(on)
        Z_PARAM_OBJ_OF_CLASS(size, sl_size_ce)
    ZEND_PARSE_PARAMETERS_END();
    zend_object *obj = Z_OBJ_P(ZEND_THIS);
    sl_obj_t *o = sl_obj(obj);
    if (!sl_ctl_need_slate(o)) {
        RETURN_THROWS();
    }
    if (!o->ctl || o->ctl->st) {
        zend_throw_error(NULL, "%s is already constructed", ZSTR_VAL(obj->ce->name));
        RETURN_THROWS();
    }
    obj->handlers->write_property(obj, s_x_name, x, NULL);
    if (!EG(exception)) {
        obj->handlers->write_property(obj, s_y_name, y, NULL);
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
    zval *slot = OBJ_PROP(obj, s_off_on);
    ZVAL_BOOL(slot, on);
    toggle_t *tg = ecalloc(1, sizeof *tg);
    zval zv;
    ZVAL_OBJ(&zv, size ? size : zend_enum_get_case_cstr(sl_size_ce, "Medium"));
    tg->size = (uint8_t)sl_size_of(&zv);
    o->ctl->st = tg;
    tg->track = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    tg->knob = sl_part_add(o, SLATE_EL_CIRCLE, -1);
    if (EG(exception) || !toggle_apply(o, false)) {   /* the layout (not realized: nothing is sent) */
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_toggle_ctor, 0, 0, 2)
    ZEND_ARG_TYPE_MASK(0, x, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_MASK(0, y, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, on, _IS_BOOL, 0, "false")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, size, Baremetal\\Slate\\Size, 0, "\\Baremetal\\Slate\\Size::Medium")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_ctl_pressed, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, down, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry toggle_methods[] = {
    PHP_ME(SlateToggle, __construct, arginfo_toggle_ctor, ZEND_ACC_PUBLIC)
    ZEND_MALIAS(SlateControl, pressed, builtinPressed, arginfo_ctl_pressed, ZEND_ACC_PROTECTED)
    PHP_FE_END
};

/* ================================ Slider ================================ */

static uint32_t s_off_value, s_off_min, s_off_max, s_off_step, s_off_fill;

typedef struct {
    uint8_t size;      /* slate_ctl_size_t */
    int w;             /* the track's length (0: only the knob) */
    int area, track, fill, knob;
    /* the touch */
    bool active;       /* Press seen and not ended (a nested Cancel inside pressed(true) leaves it false) */
    bool pending;      /* a value held back (once per frame) */
    bool framed;       /* a notification in this touch: frame holds its frame */
    uint32_t frame;    /* stats().frames at the last notification */
    int px, py;        /* the Press */
    double v0;         /* the value of before the Press */
    double said;       /* the value last notified (v0 at the Press) */
    struct {
        float w, h, ty, fw, kx, ky, kr;
        uint32_t fcolor;
    } s;               /* what was sent */
} slider_t;

static slider_t *sl_of(sl_obj_t *o)
{
    return o->ctl ? (slider_t *)o->ctl->st : NULL;
}

/* $min, $max, $step as doubles; whether all three are int (then $value is an int). */
static bool slider_range(sl_obj_t *o, double *min, double *max, double *step)
{
    zval *zmin = OBJ_PROP(&o->std, s_off_min), *zmax = OBJ_PROP(&o->std, s_off_max),
         *zstep = OBJ_PROP(&o->std, s_off_step);
    *min = zval_get_double(zmin);
    *max = zval_get_double(zmax);
    *step = zval_get_double(zstep);
    return Z_TYPE_P(zmin) == IS_LONG && Z_TYPE_P(zmax) == IS_LONG && Z_TYPE_P(zstep) == IS_LONG;
}

static double slider_value(sl_obj_t *o)
{
    return zval_get_double(OBJ_PROP(&o->std, s_off_value));
}

/* The value as $value holds it: int when the range is all int. */
static void slider_zval(sl_obj_t *o, double v, zval *out)
{
    double min, max, step;
    if (slider_range(o, &min, &max, &step)) {
        /* (double)ZEND_LONG_MAX is 2^63, out of range: clamped (ints are exact within +-2^53) */
        ZVAL_LONG(out, v >= (double)ZEND_LONG_MAX ? ZEND_LONG_MAX
                       : v <= (double)ZEND_LONG_MIN ? ZEND_LONG_MIN
                                                    : (zend_long)llround(v));
    } else {
        ZVAL_DOUBLE(out, v);
    }
}

static void slider_store(sl_obj_t *o, double v)
{
    zval *slot = OBJ_PROP(&o->std, s_off_value);
    zval_ptr_dtor(slot);
    slider_zval(o, v, slot);
}

/* Lays the slider out and sends what changed (all: everything, the realization). false with an exception. */
static bool slider_apply(sl_obj_t *o, bool all)
{
    slider_t *s = sl_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!s || !th) {
        return true;
    }
    int h = th->size_h[s->size];
    int knob = slate_ctl_slider_knob((slate_ctl_size_t)s->size);
    double min, max, step;
    slider_range(o, &min, &max, &step);
    float kx = (float)slate_ctl_slider_x(slider_value(o), s->w, knob, min, max);
    zval *fz = OBJ_PROP(&o->std, s_off_fill);
    uint32_t fcolor = sl_rgb(Z_TYPE_P(fz) == IS_LONG ? (uint32_t)Z_LVAL_P(fz) : th->c[SL_T_ACCENT]);
    float ty = (float)((h - SLATE_CTL_SLIDER_TRACK) / 2);
    if (!set_size(o, s->w, h, all)) {
        return false;
    }
    if (all) {
        const float r = SLATE_CTL_SLIDER_TRACK / 2;
        if (!sl_part_set(o, s->area, SLATE_PROP_FILL, slate_v_color(slate_ctl_alpha(0, 0)))
            || !sl_part_set(o, s->track, SLATE_PROP_FILL, slate_v_color(sl_rgb(th->c[SL_T_WELL])))
            || !sl_part_set(o, s->track, SLATE_PROP_RADIUS, slate_v_num(r))
            || !sl_part_set(o, s->track, SLATE_PROP_H, slate_v_num(SLATE_CTL_SLIDER_TRACK))
            || !sl_part_set(o, s->fill, SLATE_PROP_RADIUS, slate_v_num(r))
            || !sl_part_set(o, s->fill, SLATE_PROP_H, slate_v_num(SLATE_CTL_SLIDER_TRACK))
            || !sl_part_set(o, s->knob, SLATE_PROP_FILL, slate_v_color(sl_rgb(th->c[SL_T_TEXT])))
            || !sl_part_set(o, s->knob, SLATE_PROP_BORDER, slate_v_num(SLATE_CTL_SLIDER_BORDER))) {
            return false;
        }
    }
    float w = (float)s->w;
    bool wnew = all || s->s.w != w, tynew = all || s->s.ty != ty;
    s->s.ty = ty;
    if (!set_num(o, s->area, SLATE_PROP_W, &s->s.w, w, all) || !set_num(o, s->area, SLATE_PROP_H, &s->s.h, (float)h, all)
        || (wnew && !sl_part_set(o, s->track, SLATE_PROP_W, slate_v_num(w)))
        || (tynew && (!sl_part_set(o, s->track, SLATE_PROP_Y, slate_v_num(ty))
                      || !sl_part_set(o, s->fill, SLATE_PROP_Y, slate_v_num(ty))))
        || !set_num(o, s->fill, SLATE_PROP_W, &s->s.fw, kx, all)
        || !set_num(o, s->knob, SLATE_PROP_X, &s->s.kx, kx, all)
        || !set_num(o, s->knob, SLATE_PROP_Y, &s->s.ky, (float)h / 2, all)
        || !set_num(o, s->knob, SLATE_PROP_R, &s->s.kr, (float)knob / 2, all)) {
        return false;
    }
    if (all || s->s.fcolor != fcolor) {
        s->s.fcolor = fcolor;
        if (!sl_part_set(o, s->fill, SLATE_PROP_FILL, slate_v_color(fcolor))
            || !sl_part_set(o, s->knob, SLATE_PROP_BORDER_COLOR, slate_v_color(fcolor))) {
            return false;
        }
    }
    return true;
}

static bool slider_realize(sl_obj_t *o)
{
    return slider_apply(o, true);
}

bool sl_slider_width(sl_obj_t *o, int w)
{
    slider_t *s = o->base == SL_SLIDER ? sl_of(o) : NULL;
    if (!s) {
        return true;
    }
    w = w < 0 ? 0 : w > 32767 ? 32767 : w;
    if (s->w == w) {
        return true;
    }
    s->w = w;
    return slider_apply(o, false);   /* the knob keeps its value: placed again on the new track */
}

/* The value under local x, stored and drawn. */
static bool slider_follow(sl_obj_t *o, int lx)
{
    slider_t *s = sl_of(o);
    double min, max, step;
    slider_range(o, &min, &max, &step);
    int knob = slate_ctl_slider_knob((slate_ctl_size_t)s->size);
    slider_store(o, slate_ctl_slider_value(lx, s->w, knob, min, max, step));
    return slider_apply(o, false);
}

/* onChange() of the value now when it changed since the last one and (force: the Release) a frame went by since;
   held back otherwise. */
static bool slider_say(sl_obj_t *o, bool force)
{
    slider_t *s = sl_of(o);
    double v = slider_value(o);
    if (v == s->said) {
        s->pending = false;
        return true;
    }
    slate_engine_stats_t st;
    st.frames = 0;
    if (sl_g.engine) {
        slate_engine_stats(sl_g.engine, &st);
    }
    if (!force && s->framed && st.frames == s->frame) {
        s->pending = true;
        return true;
    }
    s->said = v;
    s->framed = true;
    s->frame = st.frames;
    s->pending = false;
    zval zv;
    slider_zval(o, v, &zv);
    return sl_ctl_notify(o, &zv);
}

static bool slider_commit(sl_obj_t *o)
{
    zval zv;
    slider_zval(o, slider_value(o), &zv);
    zend_function *fn = user_method(o, sl_slider_ce, "oncommit", sizeof("oncommit") - 1);
    if (fn) {
        zend_call_known_instance_method_with_1_params(fn, &o->std, NULL, &zv);
        if (EG(exception)) {
            return false;
        }
    }
    if (o->ctl && Z_TYPE(o->ctl->cb2) == IS_OBJECT) {
        zval cb, rv;
        ZVAL_COPY(&cb, &o->ctl->cb2);   /* kept alive through the call (committed(null) inside it) */
        ZVAL_UNDEF(&rv);
        call_user_function(NULL, NULL, &cb, &rv, 1, &zv);
        zval_ptr_dtor(&rv);
        zval_ptr_dtor(&cb);
    }
    return !EG(exception);
}

static bool slider_touch(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly)
{
    (void)ly;
    slider_t *s = sl_of(o);
    if (!s) {
        return true;
    }
    switch (kind) {
    case SL_TOUCH_PRESS:
        sl_anim_flush();
        s->active = true;
        s->pending = false;
        s->framed = false;
        s->px = p->x;
        s->py = p->y;
        s->v0 = s->said = slider_value(o);
        return slider_follow(o, lx);
    case SL_TOUCH_MOVE: {
        if (!s->active) {
            return true;
        }
        sl_anim_flush();
        if (!slider_follow(o, lx)) {
            return false;
        }
        int64_t dx = (int64_t)p->x - s->px, dy = (int64_t)p->y - s->py;
        if (!o->ctl->owned && dx * dx + dy * dy > (int64_t)SLATE_CTL_SLIDER_SLOP * SLATE_CTL_SLIDER_SLOP) {
            o->ctl->owned = true;   /* past the slop and still here: no Scroll took it */
        }
        return !o->ctl->owned || slider_say(o, false);
    }
    case SL_TOUCH_RELEASE:
        if (!s->active) {
            return true;
        }
        sl_anim_flush();
        s->active = false;
        if (!slider_follow(o, lx) || !slider_say(o, true)) {
            return false;
        }
        return slider_commit(o);
    case SL_TOUCH_CANCEL:
        if (!s->active) {
            return true;
        }
        sl_anim_flush();
        s->active = false;
        if (s->framed) {
            /* the drag already notified (an owned touch; never a hand-off to a Scroll): the value stays, a value held
               back goes out, then the commit, as at a Release where the finger stopped */
            return slider_say(o, true) && slider_commit(o);
        }
        s->pending = false;
        slider_store(o, s->v0);   /* the value of before the press (or the one the code wrote since) */
        return slider_apply(o, false);
    default:
        return true;
    }
}

/* Checks $min < $max, $step > 0 and finite numbers (name: the property written, for the finite message); false with
   a \ValueError. */
static bool slider_check(sl_obj_t *o, const char *name, zval *v)
{
    const char *cls = ZSTR_VAL(o->std.ce->name);
    double min, max, step;
    slider_range(o, &min, &max, &step);
    if (v && !isfinite(zval_get_double(v))) {
        zend_value_error("%s::$%s must be a finite number", cls, name);
        return false;
    }
    if (!(min < max)) {
        zend_value_error("%s::$min must be less than $max", cls);
        return false;
    }
    if (!(step > 0)) {
        zend_value_error("%s::$step must be greater than 0", cls);
        return false;
    }
    return true;
}

/* $value into the range and onto the step (its type from the range). */
static void slider_snap(sl_obj_t *o)
{
    double min, max, step;
    slider_range(o, &min, &max, &step);
    slider_store(o, slate_ctl_snap(slider_value(o), min, max, step));
}

static bool slider_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)old;
    if (strcmp(name, "fill") == 0) {
        if (!check_color(o, name, slot)) {
            return false;
        }
    } else if (!slider_check(o, name, slot)) {
        return false;
    } else {
        slider_snap(o);
        slider_t *s = sl_of(o);
        if (s && s->active) {
            /* from the code during a touch: no notification of it, and a Cancel comes back to it, not to the value
               of before the press */
            double min, max, step;
            slider_range(o, &min, &max, &step);
            s->said = slider_value(o);
            s->v0 = strcmp(name, "value") == 0 ? s->said : slate_ctl_snap(s->v0, min, max, step);
        }
    }
    return slider_apply(o, false);   /* sends only what changed */
}

static const sl_ctl_ops_t slider_ops = {
    .realize = slider_realize,
    .touch = slider_touch,
    .write = slider_write,
};

PHP_METHOD(SlateSlider, __construct)
{
    zval *x, *y, *min = NULL, *max = NULL, *value = NULL, *step = NULL;
    zend_long w;
    zend_object *size = NULL;
    ZEND_PARSE_PARAMETERS_START(3, 8)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_LONG(w)
        Z_PARAM_OPTIONAL
        Z_PARAM_NUMBER(min)
        Z_PARAM_NUMBER(max)
        Z_PARAM_NUMBER(value)
        Z_PARAM_NUMBER(step)
        Z_PARAM_OBJ_OF_CLASS(size, sl_size_ce)
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
    static const char *const names[4] = { "min", "max", "value", "step" };
    zval *args[4] = { min, max, value, step };
    for (int i = 0; i < 4; i++) {
        if (args[i] && !isfinite(zval_get_double(args[i]))) {
            zend_value_error("%s::$%s must be a finite number", cls, names[i]);
            RETURN_THROWS();
        }
    }
    zval *slot = OBJ_PROP(obj, s_off_min);
    zval_ptr_dtor(slot);
    if (min) {
        ZVAL_COPY(slot, min);
    } else {
        ZVAL_LONG(slot, 0);
    }
    slot = OBJ_PROP(obj, s_off_max);
    zval_ptr_dtor(slot);
    if (max) {
        ZVAL_COPY(slot, max);
    } else {
        ZVAL_LONG(slot, 100);
    }
    slot = OBJ_PROP(obj, s_off_step);
    zval_ptr_dtor(slot);
    if (step) {
        ZVAL_COPY(slot, step);
    } else {
        ZVAL_LONG(slot, 1);
    }
    slot = OBJ_PROP(obj, s_off_value);
    zval_ptr_dtor(slot);
    if (value) {
        ZVAL_COPY(slot, value);
    } else {
        ZVAL_LONG(slot, 0);
    }
    if (!slider_check(o, "value", NULL)) {
        RETURN_THROWS();
    }
    obj->handlers->write_property(obj, s_x_name, x, NULL);
    if (!EG(exception)) {
        obj->handlers->write_property(obj, s_y_name, y, NULL);
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
    slider_snap(o);
    slider_t *s = ecalloc(1, sizeof *s);
    zval zv;
    ZVAL_OBJ(&zv, size ? size : zend_enum_get_case_cstr(sl_size_ce, "Medium"));
    s->size = (uint8_t)sl_size_of(&zv);
    s->w = (int)w;
    o->ctl->st = s;
    s->area = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    s->track = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    s->fill = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    s->knob = sl_part_add(o, SLATE_EL_CIRCLE, -1);
    if (EG(exception) || !slider_apply(o, false)) {   /* the layout (not realized: nothing is sent) */
        RETURN_THROWS();
    }
}

PHP_METHOD(SlateSlider, onCommit)
{
    zval *v;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_NUMBER(v)
    ZEND_PARSE_PARAMETERS_END();
    (void)v;
}

PHP_METHOD(SlateSlider, committed)
{
    zval *fn = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(fn, zend_ce_closure)
    ZEND_PARSE_PARAMETERS_END();
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (o->ctl) {
        zval old;
        ZVAL_COPY_VALUE(&old, &o->ctl->cb2);
        if (fn) {
            ZVAL_COPY(&o->ctl->cb2, fn);
        } else {
            ZVAL_UNDEF(&o->ctl->cb2);
        }
        zval_ptr_dtor(&old);   /* after: it may be running */
    }
    RETURN_OBJ_COPY(&o->std);
}

#define T_NUM (MAY_BE_LONG | MAY_BE_DOUBLE)

ZEND_BEGIN_ARG_INFO_EX(arginfo_slider_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, x, T_NUM, NULL)
    ZEND_ARG_TYPE_MASK(0, y, T_NUM, NULL)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_MASK(0, min, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, max, T_NUM, "100")
    ZEND_ARG_TYPE_MASK(0, value, T_NUM, "0")
    ZEND_ARG_TYPE_MASK(0, step, T_NUM, "1")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, size, Baremetal\\Slate\\Size, 0, "\\Baremetal\\Slate\\Size::Medium")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_slider_on_commit, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_MASK(0, value, T_NUM, NULL)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_slider_committed, 0, 1, IS_STATIC, 0)
    ZEND_ARG_OBJ_INFO(0, fn, Closure, 1)
ZEND_END_ARG_INFO()

static const zend_function_entry slider_methods[] = {
    PHP_ME(SlateSlider, __construct, arginfo_slider_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateSlider, onCommit, arginfo_slider_on_commit, ZEND_ACC_PUBLIC)
    PHP_ME(SlateSlider, committed, arginfo_slider_committed, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ================================ registration ================================ */

static uint32_t declare(zend_class_entry *ce, const char *name, zend_type t, zval *dv)
{
    zval undef;
    ZVAL_UNDEF(&undef);
    zend_property_info *pi = zend_declare_typed_property(ce, zend_string_init_interned(name, strlen(name), 1),
                                                         dv ? dv : &undef, ZEND_ACC_PUBLIC, NULL, t);
    return pi->offset;
}

void sl_toggle_register(void)
{
    s_x_name = zend_string_init_interned("x", 1, 1);
    s_y_name = zend_string_init_interned("y", 1, 1);
    zend_class_entry ce;
    zval dv;

    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Toggle", toggle_methods);
    sl_toggle_ce = zend_register_internal_class_ex(&ce, sl_control_ce);   /* not final: an app may extend it */
    sl_toggle_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    ZVAL_FALSE(&dv);
    s_off_on = declare(sl_toggle_ce, "on", (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0), &dv);
    /* a comfortable target: Element's $hitPad redeclared (its slot) */
    ZVAL_LONG(&dv, 8);
    declare(sl_toggle_ce, "hitPad", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0), &dv);
    sl_ctl_class(sl_toggle_ce, SL_TOGGLE, &toggle_ops);

    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Slider", slider_methods);
    sl_slider_ce = zend_register_internal_class_ex(&ce, sl_control_ce);
    sl_slider_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    zend_type num = (zend_type)ZEND_TYPE_INIT_MASK(MAY_BE_LONG | MAY_BE_DOUBLE);
    s_off_value = declare(sl_slider_ce, "value", num, NULL);
    s_off_min = declare(sl_slider_ce, "min", num, NULL);
    s_off_max = declare(sl_slider_ce, "max", num, NULL);
    s_off_step = declare(sl_slider_ce, "step", num, NULL);
    ZVAL_NULL(&dv);
    s_off_fill = declare(sl_slider_ce, "fill", (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 1, 0), &dv);
    sl_ctl_class(sl_slider_ce, SL_SLIDER, &slider_ops);
}

#endif
