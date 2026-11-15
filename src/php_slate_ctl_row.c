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
 * SettingRow (a Control, php_slate_control.c): a row of a settings list, w wide, its height from slate_ctl_row().
 * Its parts, bottom to top:
 *   - bg: a ROUNDRECT w x h, the text colour at alpha 0; pressed (only a touchable row gets touches): the text colour
 *     at 4 %, its FILL easing in SLATE_CTL_PRESS_MS;
 *   - div: a ROUNDRECT (0, h - 1, w, 1) in the border colour, the divider;
 *   - label: a TEXT in the theme's font and text colour, at the left;
 *   - sub: a TEXT in fontSmall and textMuted under the label (empty: none);
 *   - value: a TEXT in fontMono and textMuted, right-aligned at w.
 * The control (any Element, usually one of Slate's controls) is a PHP child of the row (sl_children_add): it gets its
 * own touches and is drawn above the parts. The row places it (writes its x and y, as the code would): at the right,
 * left of the value, centred in the height. A Slider made with w 0 is stretched instead: its track gets the space
 * between the label (at most 40 % of w) and the value (sl_slider_width). A write of $label, $sub or $value sends the
 * new text and lays the row out again (the control too).
 *
 * $touchable is false by default (the class redeclares Element's): a touch outside the control reaches what is under
 * the row. With $touchable or a callback of on() the row is a touchable like any other, with the pressed look.
 */
#include <string.h>

#include "php.h"
#include "zend_exceptions.h"

#include "controls.h"
#include "php_slate.h"
#include "raster.h"

#if SLATE_HAS_CONTROLS

static zend_class_entry *sl_row_ce;
static uint32_t s_off_label, s_off_sub, s_off_value;
static zend_string *s_x_name, *s_y_name;

typedef struct {
    sl_obj_t *control;   /* the control (a child of the row, which holds its reference); NULL: none */
    bool stretch;        /* the control is a Slider made with w 0 */
    bool down;           /* the pressed look */
    int w;
    int bg, div, label, sub, value;   /* part indexes */
    struct {
        float h, ly, sy, vy;
        uint32_t fill;
    } s;                 /* what was sent */
} row_t;

static row_t *row_of(sl_obj_t *o)
{
    return o->ctl ? (row_t *)o->ctl->st : NULL;
}

static const char *str_of(sl_obj_t *o, uint32_t off)
{
    zval *z = OBJ_PROP(&o->std, off);
    return Z_TYPE_P(z) == IS_STRING ? Z_STRVAL_P(z) : "";
}

/* The width and the line height of str in font (0, 0 without a font; width 0 for an empty string). */
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

/* The laid-out size: ctl->w / h, the $w / $h slots and, realized and not at the realization, the group's W and H. */
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

/* Puts the control's top-left (its box, sl_obj_box) at (x, y): a write of its x and y. */
static bool place(sl_obj_t *c, int x, int y)
{
    int x0, y0, x1, y1;
    int cx = (int)sl_slot_num(c, "x"), cy = (int)sl_slot_num(c, "y");
    if (!sl_obj_box(c, &x0, &y0, &x1, &y1)) {
        x0 = cx;
        y0 = cy;
    }
    int nx = x + (cx - x0), ny = y + (cy - y0);
    if (nx != cx && !sl_write_num(c, "x", nx)) {
        return false;
    }
    return ny == cy || sl_write_num(c, "y", ny);
}

/* Lays the row out and sends what changed (all: everything, the realization: the control is a child, realized with
   the place it has). texts: a text changed (all: every one is sent). false with an exception. */
static bool apply(sl_obj_t *o, bool all, bool texts)
{
    row_t *r = row_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!r || !th) {
        return true;
    }
    const char *label = str_of(o, s_off_label), *sub = str_of(o, s_off_sub), *value = str_of(o, s_off_value);
    int lw, llh, sw, slh, vw, vlh;
    measure(label, th->font, &lw, &llh);
    measure(sub, th->font_small, &sw, &slh);
    measure(value, th->font_mono, &vw, &vlh);
    if (!sub[0]) {
        sw = slh = 0;
    }
    int cw = 0, ch = 0;
    sl_obj_t *c = r->control;
    if (c && c->parent == o) {
        int x0, y0, x1, y1;
        if (SL_CTL(c) && c->ctl->ops) {
            cw = r->stretch ? 0 : c->ctl->w;
            ch = c->ctl->h;
        } else if (sl_obj_box(c, &x0, &y0, &x1, &y1)) {
            cw = x1 - x0;
            ch = y1 - y0;
        }
    } else {
        c = NULL;
    }
    slate_ctl_row_t L = slate_ctl_row(r->w, lw, llh, sw, slh, vw, cw, ch, r->stretch && c);
    if (!set_size(o, r->w, L.h, all)) {
        return false;
    }
    const uint32_t *t = th->c;
    float h = (float)L.h, w = (float)r->w;
    uint32_t fill = slate_ctl_alpha(t[SL_T_TEXT], r->down ? 4 : 0);
    if (all) {
        if (!sl_part_set(o, r->bg, SLATE_PROP_W, slate_v_num(w))
            || !sl_part_set(o, r->bg, SLATE_PROP_FILL, slate_v_color(fill))
            || !sl_part_ease(o, r->bg, 1u << SLATE_PROP_FILL, SLATE_CTL_PRESS_MS, SLATE_EASE_OUT_CUBIC)
            || !sl_part_set(o, r->div, SLATE_PROP_W, slate_v_num(w))
            || !sl_part_set(o, r->div, SLATE_PROP_H, slate_v_num(1))
            || !sl_part_set(o, r->div, SLATE_PROP_FILL, slate_v_color(sl_rgb(t[SL_T_BORDER])))
            || !sl_part_set(o, r->label, SLATE_PROP_FONT, slate_v_font(th->font))
            || !sl_part_set(o, r->label, SLATE_PROP_COLOR, slate_v_color(sl_rgb(t[SL_T_TEXT])))
            || !sl_part_set(o, r->sub, SLATE_PROP_FONT, slate_v_font(th->font_small))
            || !sl_part_set(o, r->sub, SLATE_PROP_COLOR, slate_v_color(sl_rgb(t[SL_T_TEXT_MUTED])))
            || !sl_part_set(o, r->value, SLATE_PROP_FONT, slate_v_font(th->font_mono))
            || !sl_part_set(o, r->value, SLATE_PROP_COLOR, slate_v_color(sl_rgb(t[SL_T_TEXT_MUTED])))
            || !sl_part_set(o, r->value, SLATE_PROP_ALIGN, slate_v_enum(SLATE_ALIGN_RIGHT))
            || !sl_part_set(o, r->value, SLATE_PROP_X, slate_v_num(w))) {
            return false;
        }
        r->s.fill = fill;
    } else if (r->s.fill != fill) {
        r->s.fill = fill;
        if (!sl_part_set(o, r->bg, SLATE_PROP_FILL, slate_v_color(fill))) {
            return false;
        }
    }
    bool hnew = all || r->s.h != h;
    if (!set_num(o, r->bg, SLATE_PROP_H, &r->s.h, h, all)
        || (hnew && !sl_part_set(o, r->div, SLATE_PROP_Y, slate_v_num(h - 1)))
        || !set_num(o, r->label, SLATE_PROP_Y, &r->s.ly, (float)L.label_y, all)
        || !set_num(o, r->sub, SLATE_PROP_Y, &r->s.sy, (float)L.sub_y, all)
        || !set_num(o, r->value, SLATE_PROP_Y, &r->s.vy, (float)((L.h - vlh) / 2), all)) {
        return false;
    }
    if ((all || texts)
        && (!sl_part_set(o, r->label, SLATE_PROP_TEXT, slate_v_text(label))
            || !sl_part_set(o, r->sub, SLATE_PROP_TEXT, slate_v_text(sub))
            || !sl_part_set(o, r->value, SLATE_PROP_TEXT, slate_v_text(value)))) {
        return false;
    }
    if (all || !c) {
        return true;   /* the realization sends the parts only: the control is placed already */
    }
    if (r->stretch) {
        if (!sl_slider_width(c, L.slider_w)) {
            return false;
        }
        return place(c, L.slider_x, L.ctl_y);
    }
    return place(c, L.ctl_x, L.ctl_y);
}

/* ---- ops ---- */

static bool row_realize(sl_obj_t *o)
{
    return apply(o, true, true);
}

static void row_pressed(sl_obj_t *o, bool down)
{
    row_t *r = row_of(o);
    if (r && r->down != down) {
        r->down = down;
        apply(o, false, false);   /* an exception (no memory for the change) is the caller's */
    }
}

static bool row_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)name;
    if (zend_is_identical(old, slot)) {
        return true;
    }
    return apply(o, false, true);   /* $label, $sub or $value */
}

static const sl_ctl_ops_t row_ops = {
    .realize = row_realize,
    .pressed = row_pressed,
    .write = row_write,
};

/* ---- SettingRow ---- */

PHP_METHOD(SlateSettingRow, __construct)
{
    zval *x, *y;
    zend_long w;
    zend_string *label, *sub = NULL, *value = NULL;
    zend_object *control = NULL;
    ZEND_PARSE_PARAMETERS_START(4, 7)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_LONG(w)
        Z_PARAM_STR(label)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(sub)
        Z_PARAM_OBJ_OF_CLASS_OR_NULL(control, sl_element_ce)
        Z_PARAM_STR(value)
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
    if (w < 1 || w > 32767) {
        zend_value_error("%s::$w must be between 1 and 32767", cls);
        RETURN_THROWS();
    }
    obj->handlers->write_property(obj, s_x_name, x, NULL);
    if (!EG(exception)) {
        obj->handlers->write_property(obj, s_y_name, y, NULL);
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
    zend_string *strs[3] = { label, sub, value };
    uint32_t offs[3] = { s_off_label, s_off_sub, s_off_value };
    for (int i = 0; i < 3; i++) {
        zval *slot = OBJ_PROP(obj, offs[i]);
        zval_ptr_dtor(slot);
        if (strs[i]) {
            ZVAL_STR_COPY(slot, strs[i]);
        } else {
            ZVAL_EMPTY_STRING(slot);
        }
    }
    row_t *r = ecalloc(1, sizeof *r);
    r->w = (int)w;
    o->ctl->st = r;
    r->bg = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    r->div = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    r->label = sl_part_add(o, SLATE_EL_TEXT, -1);
    r->sub = sl_part_add(o, SLATE_EL_TEXT, -1);
    r->value = sl_part_add(o, SLATE_EL_TEXT, -1);
    if (EG(exception)) {
        RETURN_THROWS();
    }
    if (control) {
        sl_obj_t *c = sl_obj(control);
        zval zv;
        ZVAL_OBJ(&zv, control);
        sl_children_add(o, &zv, 1);   /* already in a group: its \LogicException */
        if (EG(exception)) {
            RETURN_THROWS();
        }
        r->control = c;
        r->stretch = c->base == SL_SLIDER && SL_CTL(c) && c->ctl->w == 0;
    }
    if (!apply(o, false, false)) {   /* the layout (not realized: nothing is sent), the control placed */
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_row_ctor, 0, 0, 4)
    ZEND_ARG_TYPE_MASK(0, x, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_MASK(0, y, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, label, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, sub, IS_STRING, 0, "''")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, control, Baremetal\\Slate\\Element, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, value, IS_STRING, 0, "''")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_row_pressed, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, down, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry row_methods[] = {
    PHP_ME(SlateSettingRow, __construct, arginfo_row_ctor, ZEND_ACC_PUBLIC)
    ZEND_MALIAS(SlateControl, pressed, builtinPressed, arginfo_row_pressed, ZEND_ACC_PROTECTED)
    PHP_FE_END
};

static uint32_t declare_str(zend_class_entry *ce, const char *name)
{
    zval dv;
    ZVAL_UNDEF(&dv);
    zend_property_info *pi = zend_declare_typed_property(ce, zend_string_init_interned(name, strlen(name), 1), &dv,
                                                         ZEND_ACC_PUBLIC, NULL,
                                                         (zend_type)ZEND_TYPE_INIT_CODE(IS_STRING, 0, 0));
    return pi->offset;
}

void sl_row_register(void)
{
    s_x_name = zend_string_init_interned("x", 1, 1);
    s_y_name = zend_string_init_interned("y", 1, 1);
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "SettingRow", row_methods);
    sl_row_ce = zend_register_internal_class_ex(&ce, sl_control_ce);   /* not final: an app may extend it */
    sl_row_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    s_off_label = declare_str(sl_row_ce, "label");
    s_off_sub = declare_str(sl_row_ce, "sub");
    s_off_value = declare_str(sl_row_ce, "value");
    /* not a touchable by default: Element's $touchable redeclared (its slot) */
    zval dv;
    ZVAL_FALSE(&dv);
    zend_declare_typed_property(sl_row_ce, zend_string_init_interned("touchable", sizeof("touchable") - 1, 1), &dv,
                                ZEND_ACC_PUBLIC, NULL, (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0));
    sl_ctl_class(sl_row_ce, SL_SETTING_ROW, &row_ops);
}

#endif
