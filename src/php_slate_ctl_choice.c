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
 * Choice (a Control, php_slate_control.c): one or more items picked among labels, in four styles (ChoiceStyle). Its
 * parts, bottom to top:
 *   - well (Segmented only): a ROUNDRECT 0..w x h, fill well, a border of 1 in border, radius the theme's + 4;
 *   - for each item: bg, a ROUNDRECT on the item's box (controls.h: slate_ctl_segments(), _chips(), _rows(), _nav());
 *     icon, an IMAGE (with $icons only), tinted with the item's text colour (always at alpha 255: a tint never goes
 *     to or from none); label, a TEXT (the theme's font; Nav: fontSmall);
 *   - bar (Nav only): a ROUNDRECT 3 high, accent, over the selected part (slate_ctl_nav_bar()); its X eases.
 * The colours of an item (fill, border, text) follow the style and whether it is selected; bg eases FILL and
 * BORDER_COLOR, label COLOR, icon TINT, all in SLATE_CTL_CHOICE_MS (OutCubic), after the first values (the first
 * look is at once). Pressed (from Press to Release / Cancel, the item under the finger): slate_ctl_pressed() (an
 * opaque fill 12 % toward white; one that is not opaque gets the text colour at 6 % laid over it), eased in
 * SLATE_CTL_PRESS_MS: the bg's EASE is switched to it for a change of the pressed look and back to
 * SLATE_CTL_CHOICE_MS for a change of the selection. The touch point is brought into the items first for the
 * contiguous styles (Segmented, List, Nav: the well's padding and $hitPad pick the nearest item); Chips keep their
 * gaps. The Nav bar's X and W ease; shown again after none was selected, it jumps to its part.
 * The touch: a Tap on the item that the Press hit selects it (without $multi: when it is not the selected one,
 * $selected = i and onChange(i); with $multi: it is turned on or off and onChange() gets the sorted list). A write
 * of $selected from the code slides the colours, without onChange(). The labels, the icons, the style and the size
 * are fixed at the construction, so the geometry is sent once (at the realization); later only colours and the bar.
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

static zend_class_entry *sl_choice_ce;
static uint32_t s_off_selected;
static zend_string *s_x_name, *s_y_name;

enum { ST_SEGMENTED, ST_CHIPS, ST_LIST, ST_NAV };
static const char *const style_names[] = { "Segmented", "Chips", "List", "Nav" };

/* What was sent to an item's parts (colours: the geometry is sent once). */
typedef struct {
    uint32_t fill, bcolor, tcolor;
    uint16_t ms;            /* the bg's EASE */
} item_sent_t;

typedef struct {
    uint8_t style, size;
    bool multi;
    bool down;              /* pressed() true and not ended */
    int n;
    int wgiven;
    int press;              /* the item the Press hit (-1: none) */
    zend_string **labels;   /* n */
    slate_image_t **icons;  /* n, or NULL: no icons */
    bool *on;               /* n: selected */
    slate_ctl_box_t *box;   /* n: the items, local */
    item_sent_t *s;         /* n */
    int well, bar;          /* part indexes (-1: none); item i: bg first + 3 i (2 i without icons) */
    int first;
    float bar_x, bar_w;     /* sent */
    uint32_t bar_fill;
} choice_t;

static choice_t *ch_of(sl_obj_t *o)
{
    return o->ctl ? (choice_t *)o->ctl->st : NULL;
}

static int part_bg(const choice_t *ch, int i)
{
    return ch->first + i * (ch->icons ? 3 : 2);
}

static int part_icon(const choice_t *ch, int i)
{
    return ch->icons ? part_bg(ch, i) + 1 : -1;
}

static int part_label(const choice_t *ch, int i)
{
    return part_bg(ch, i) + (ch->icons ? 2 : 1);
}

/* ---- $selected ---- */

/* The on flags from $selected (already checked). */
static void read_selected(sl_obj_t *o, choice_t *ch)
{
    zval *slot = OBJ_PROP(&o->std, s_off_selected);
    memset(ch->on, 0, (size_t)ch->n * sizeof *ch->on);
    if (Z_TYPE_P(slot) == IS_LONG) {
        zend_long i = Z_LVAL_P(slot);
        if (i >= 0 && i < ch->n) {
            ch->on[i] = true;
        }
    } else if (Z_TYPE_P(slot) == IS_ARRAY) {
        zval *v;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(slot), v) {
            if (Z_TYPE_P(v) == IS_LONG && Z_LVAL_P(v) >= 0 && Z_LVAL_P(v) < ch->n) {
                ch->on[Z_LVAL_P(v)] = true;
            }
        } ZEND_HASH_FOREACH_END();
    }
}

/* The sorted list of the on items into rv. */
static void list_of(const choice_t *ch, zval *rv)
{
    array_init(rv);
    for (int i = 0; i < ch->n; i++) {
        if (ch->on[i]) {
            add_next_index_long(rv, i);
        }
    }
}

/* Checks v as $selected of a choice of n items and puts its normal form in out: an int without multi; with multi a
   sorted list without repeats (an int i is [i]). false with the \ValueError. */
static bool normal_selected(const char *cls, int n, bool multi, zval *v, zval *out)
{
    if (!multi) {
        if (Z_TYPE_P(v) != IS_LONG || Z_LVAL_P(v) < 0 || Z_LVAL_P(v) >= n) {
            zend_value_error("%s::$selected must be between 0 and %d", cls, n - 1);
            return false;
        }
        ZVAL_LONG(out, Z_LVAL_P(v));
        return true;
    }
    bool *on = ecalloc((size_t)n, sizeof *on);
    bool ok = true;
    if (Z_TYPE_P(v) == IS_LONG) {
        ok = Z_LVAL_P(v) >= 0 && Z_LVAL_P(v) < n;
        if (ok) {
            on[Z_LVAL_P(v)] = true;
        }
    } else if (Z_TYPE_P(v) == IS_ARRAY && zend_array_is_list(Z_ARRVAL_P(v))) {
        zval *e;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), e) {
            if (Z_TYPE_P(e) != IS_LONG || Z_LVAL_P(e) < 0 || Z_LVAL_P(e) >= n) {
                ok = false;
                break;
            }
            on[Z_LVAL_P(e)] = true;
        } ZEND_HASH_FOREACH_END();
    } else {
        ok = false;
    }
    if (!ok) {
        efree(on);
        zend_value_error("%s::$selected must be a list of indexes between 0 and %d", cls, n - 1);
        return false;
    }
    array_init(out);
    for (int i = 0; i < n; i++) {
        if (on[i]) {
            add_next_index_long(out, i);
        }
    }
    efree(on);
    return true;
}

/* ---- the look ---- */

/* The fill, the border colour and the text colour of item i (0xAARRGGBB). */
static void colours(const choice_t *ch, const sl_theme_t *th, int i, uint32_t *fill, uint32_t *bcolor,
                    uint32_t *text)
{
    const uint32_t *c = th->c;
    bool on = ch->on[i];
    uint32_t clear = slate_ctl_alpha(c[SL_T_ACCENT], 0);
    switch (ch->style) {
    case ST_SEGMENTED:
        *fill = *bcolor = on ? sl_rgb(c[SL_T_ACCENT]) : clear;
        *text = sl_rgb(on ? c[SL_T_ON_ACCENT] : c[SL_T_TEXT_MUTED]);
        break;
    case ST_CHIPS:
        *fill = sl_rgb(on ? c[SL_T_ACCENT] : c[SL_T_RAISED]);
        *bcolor = sl_rgb(on ? c[SL_T_ACCENT] : c[SL_T_BORDER]);
        *text = sl_rgb(on ? c[SL_T_ON_ACCENT] : c[SL_T_TEXT_MUTED]);
        break;
    case ST_LIST:
        *fill = on ? slate_ctl_alpha(c[SL_T_ACCENT], 12) : clear;
        *bcolor = on ? slate_ctl_alpha(c[SL_T_ACCENT], 35) : clear;
        *text = sl_rgb(on ? c[SL_T_ACCENT] : c[SL_T_TEXT_MUTED]);
        break;
    default:   /* Nav */
        *fill = *bcolor = clear;
        *text = sl_rgb(on ? c[SL_T_ACCENT] : c[SL_T_TEXT_DIM]);
        break;
    }
    if (ch->down && ch->press == i) {
        *fill = slate_ctl_pressed(*fill, c[SL_T_TEXT]);
    }
}

static bool set_num(sl_obj_t *o, int part, slate_prop_t prop, float v)
{
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

/* The laid-out size: ctl->w / h and the $w / $h slots (the group's W and H go with the realization). */
static void set_size(sl_obj_t *o, int w, int h)
{
    w = w > 32767 ? 32767 : w;
    h = h > 32767 ? 32767 : h;
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

static const struct display_font_entry *font_of(const choice_t *ch, const sl_theme_t *th)
{
    return ch->style == ST_NAV ? th->font_small : th->font;
}

/* The line height of the font (0: none). */
static int line_h(const struct display_font_entry *f)
{
    return f ? slate_text_bbox(0, 0, " ", f, SLATE_ALIGN_LEFT, 0).y1 : 0;
}

static int text_w(const struct display_font_entry *f, zend_string *s)
{
    if (!f || ZSTR_LEN(s) == 0) {
        return 0;
    }
    slate_rect_t r = slate_text_bbox(0, 0, ZSTR_VAL(s), f, SLATE_ALIGN_LEFT, 0);
    return r.x1 - r.x0;
}

/* The layout (the items' boxes and the size): at the construction. */
static void layout(sl_obj_t *o, choice_t *ch, const sl_theme_t *th)
{
    const struct display_font_entry *f = font_of(ch, th);
    int h = th->size_h[ch->size], pad = slate_ctl_pad((slate_ctl_size_t)ch->size);
    int lead = ch->icons ? SLATE_CTL_ICON + SLATE_CTL_ICON_GAP : 0;
    int *iw = emalloc((size_t)ch->n * sizeof *iw);
    for (int i = 0; i < ch->n; i++) {
        iw[i] = text_w(f, ch->labels[i]) + 2 * pad + lead;
    }
    int w = ch->wgiven, total_h = h;
    switch (ch->style) {
    case ST_SEGMENTED:
        w = slate_ctl_segments(ch->wgiven, ch->n, iw, h, ch->box);
        break;
    case ST_CHIPS:
        total_h = slate_ctl_chips(ch->wgiven, ch->n, iw, h, ch->box);
        if (!ch->wgiven) {
            w = ch->box[ch->n - 1].x + ch->box[ch->n - 1].w;
        }
        break;
    case ST_LIST:
        slate_ctl_rows(ch->wgiven, ch->n, h, ch->box);
        total_h = ch->n * h;
        break;
    default:
        slate_ctl_nav(ch->wgiven, ch->n, h, ch->box);
        break;
    }
    efree(iw);
    set_size(o, w, total_h);
}

/* The geometry of every part and the first colours (the realization), then the EASEs. false with an exception. */
static bool send_all(sl_obj_t *o, choice_t *ch, const sl_theme_t *th)
{
    const struct display_font_entry *f = font_of(ch, th);
    int pad = slate_ctl_pad((slate_ctl_size_t)ch->size), lh = line_h(f);
    int lead = ch->icons ? SLATE_CTL_ICON + SLATE_CTL_ICON_GAP : 0;
    float radius = (float)th->radius;
    if (ch->well >= 0) {
        if (!set_num(o, ch->well, SLATE_PROP_W, (float)o->ctl->w) || !set_num(o, ch->well, SLATE_PROP_H, (float)o->ctl->h)
            || !set_num(o, ch->well, SLATE_PROP_RADIUS, radius + 4)
            || !sl_part_set(o, ch->well, SLATE_PROP_FILL, slate_v_color(sl_rgb(th->c[SL_T_WELL])))
            || !set_num(o, ch->well, SLATE_PROP_BORDER, 1)
            || !sl_part_set(o, ch->well, SLATE_PROP_BORDER_COLOR, slate_v_color(sl_rgb(th->c[SL_T_BORDER])))) {
            return false;
        }
    }
    for (int i = 0; i < ch->n; i++) {
        const slate_ctl_box_t *b = &ch->box[i];
        int bg = part_bg(ch, i), ic = part_icon(ch, i), lb = part_label(ch, i);
        int tw = text_w(f, ch->labels[i]);
        float r = ch->style == ST_CHIPS ? (float)b->h / 2 : radius;
        float border = ch->style == ST_CHIPS || ch->style == ST_LIST ? 1.0f : 0.0f;
        float ix, iy, lx, ly;
        if (ch->style == ST_NAV) {
            int top = (b->h - (SLATE_CTL_NAV_ICON + 2 + lh)) / 2;
            top = top < 5 ? 5 : top;   /* below the bar (3) and a gap */
            ix = (float)(b->x + (b->w - SLATE_CTL_NAV_ICON) / 2);
            iy = (float)(b->y + top);
            lx = (float)(b->x + (b->w - tw) / 2);
            ly = (float)(b->y + top + SLATE_CTL_NAV_ICON + 2);
        } else {
            int x0 = ch->style == ST_LIST ? b->x + pad : b->x + (b->w - tw - lead) / 2;
            ix = (float)x0;
            iy = (float)(b->y + (b->h - SLATE_CTL_ICON) / 2);
            lx = (float)(x0 + lead);
            ly = (float)(b->y + (b->h - lh) / 2);
        }
        uint32_t fill, bcolor, tcolor;
        colours(ch, th, i, &fill, &bcolor, &tcolor);
        item_sent_t *s = &ch->s[i];
        s->ms = SLATE_CTL_CHOICE_MS;
        if (!set_num(o, bg, SLATE_PROP_X, b->x) || !set_num(o, bg, SLATE_PROP_Y, b->y)
            || !set_num(o, bg, SLATE_PROP_W, b->w) || !set_num(o, bg, SLATE_PROP_H, b->h)
            || !set_num(o, bg, SLATE_PROP_RADIUS, r) || !set_num(o, bg, SLATE_PROP_BORDER, border)
            || !set_color(o, bg, SLATE_PROP_FILL, &s->fill, fill, true)
            || !set_color(o, bg, SLATE_PROP_BORDER_COLOR, &s->bcolor, bcolor, true)
            || !sl_part_ease(o, bg, (1u << SLATE_PROP_FILL) | (1u << SLATE_PROP_BORDER_COLOR), SLATE_CTL_CHOICE_MS,
                             SLATE_EASE_OUT_CUBIC)) {
            return false;
        }
        if (ic >= 0
            && (!sl_part_set(o, ic, SLATE_PROP_IMAGE, slate_v_image(ch->icons[i])) || !set_num(o, ic, SLATE_PROP_X, ix)
                || !set_num(o, ic, SLATE_PROP_Y, iy) || !sl_part_set(o, ic, SLATE_PROP_TINT, slate_v_color(tcolor))
                || !sl_part_ease(o, ic, 1u << SLATE_PROP_TINT, SLATE_CTL_CHOICE_MS, SLATE_EASE_OUT_CUBIC))) {
            return false;
        }
        if (!sl_part_set(o, lb, SLATE_PROP_TEXT, slate_v_text(ZSTR_VAL(ch->labels[i])))
            || !sl_part_set(o, lb, SLATE_PROP_FONT, slate_v_font(f)) || !set_num(o, lb, SLATE_PROP_X, lx)
            || !set_num(o, lb, SLATE_PROP_Y, ly) || !set_color(o, lb, SLATE_PROP_COLOR, &s->tcolor, tcolor, true)
            || !sl_part_ease(o, lb, 1u << SLATE_PROP_COLOR, SLATE_CTL_CHOICE_MS, SLATE_EASE_OUT_CUBIC)) {
            return false;
        }
    }
    return true;
}

/* The bar of a Nav: over the first selected part (none: accent at alpha 0, where it was). */
static bool send_bar(sl_obj_t *o, choice_t *ch, const sl_theme_t *th, bool all)
{
    if (ch->bar < 0) {
        return true;
    }
    int sel = -1;
    for (int i = 0; i < ch->n && sel < 0; i++) {
        sel = ch->on[i] ? i : -1;
    }
    uint32_t fill = sel >= 0 ? sl_rgb(th->c[SL_T_ACCENT]) : slate_ctl_alpha(th->c[SL_T_ACCENT], 0);
    bool hidden = (ch->bar_fill >> 24) == 0;   /* before this change */
    const uint32_t mask = (1u << SLATE_PROP_X) | (1u << SLATE_PROP_W);
    if (all || sel >= 0) {
        slate_ctl_box_t bar = slate_ctl_nav_bar(&ch->box[sel >= 0 ? sel : 0]);
        if (all || ch->bar_x != (float)bar.x || ch->bar_w != (float)bar.w) {
            /* shown again after none was selected: at its part at once, not sliding from where it was hidden */
            bool jump = !all && hidden && sl_realized(o);
            ch->bar_x = (float)bar.x;
            ch->bar_w = (float)bar.w;
            if ((jump && !sl_part_ease(o, ch->bar, 0, 0, SLATE_EASE_LINEAR))
                || !set_num(o, ch->bar, SLATE_PROP_X, (float)bar.x) || !set_num(o, ch->bar, SLATE_PROP_W, (float)bar.w)
                || (jump && !sl_part_ease(o, ch->bar, mask, SLATE_CTL_CHOICE_MS, SLATE_EASE_OUT_CUBIC))) {
                return false;
            }
        }
    }
    if (!set_color(o, ch->bar, SLATE_PROP_FILL, &ch->bar_fill, fill, all)) {
        return false;
    }
    if (all
        && (!set_num(o, ch->bar, SLATE_PROP_Y, 0) || !set_num(o, ch->bar, SLATE_PROP_H, 3)
            || !set_num(o, ch->bar, SLATE_PROP_RADIUS, 1.5f)
            || !sl_part_ease(o, ch->bar, mask, SLATE_CTL_CHOICE_MS, SLATE_EASE_OUT_CUBIC))) {
        return false;
    }
    return true;
}

/* The colours that changed (all: everything, the realization). press: a change of the pressed look (the bg eases in
   SLATE_CTL_PRESS_MS), else of the selection (SLATE_CTL_CHOICE_MS). false with an exception. */
static bool apply(sl_obj_t *o, bool all, bool press)
{
    choice_t *ch = ch_of(o);
    sl_theme_t *th = sl_ctl_theme(o);
    if (!ch || !th) {
        return true;
    }
    read_selected(o, ch);
    if (all) {
        return send_all(o, ch, th) && send_bar(o, ch, th, true);
    }
    uint16_t ms = press ? SLATE_CTL_PRESS_MS : SLATE_CTL_CHOICE_MS;
    for (int i = 0; i < ch->n; i++) {
        uint32_t fill, bcolor, tcolor;
        colours(ch, th, i, &fill, &bcolor, &tcolor);
        item_sent_t *s = &ch->s[i];
        int bg = part_bg(ch, i), ic = part_icon(ch, i);
        if ((s->fill != fill || s->bcolor != bcolor) && s->ms != ms && sl_realized(o)) {
            s->ms = ms;
            if (!sl_part_ease(o, bg, (1u << SLATE_PROP_FILL) | (1u << SLATE_PROP_BORDER_COLOR), ms,
                              SLATE_EASE_OUT_CUBIC)) {
                return false;
            }
        }
        if (!set_color(o, bg, SLATE_PROP_FILL, &s->fill, fill, false)
            || !set_color(o, bg, SLATE_PROP_BORDER_COLOR, &s->bcolor, bcolor, false)) {
            return false;
        }
        if (s->tcolor != tcolor) {
            s->tcolor = tcolor;
            if (!sl_part_set(o, part_label(ch, i), SLATE_PROP_COLOR, slate_v_color(tcolor))
                || (ic >= 0 && !sl_part_set(o, ic, SLATE_PROP_TINT, slate_v_color(tcolor)))) {
                return false;
            }
        }
    }
    return send_bar(o, ch, th, false);
}

/* ---- ops ---- */

static bool choice_realize(sl_obj_t *o)
{
    return apply(o, true, false);
}

static void choice_pressed(sl_obj_t *o, bool down)
{
    choice_t *ch = ch_of(o);
    if (!ch) {
        return;
    }
    if (down) {
        ch->press = -1;   /* the Press's touch (after this) finds the item under the finger */
    }
    if (ch->down != down) {
        ch->down = down;
        apply(o, false, true);   /* an exception (no memory for the change) is the caller's */
    }
}

/* The item under local (x, y); for the contiguous styles (all but Chips) the point is brought into the items first,
   so the well's padding and $hitPad pick the nearest item. -1: none. */
static int item_under(const choice_t *ch, int x, int y)
{
    if (ch->style != ST_CHIPS) {
        const slate_ctl_box_t *a = &ch->box[0], *z = &ch->box[ch->n - 1];
        int x1 = z->x + z->w - 1, y1 = z->y + z->h - 1;
        x = x < a->x ? a->x : x > x1 ? x1 : x;
        y = y < a->y ? a->y : y > y1 ? y1 : y;
    }
    return slate_ctl_item_at(ch->box, ch->n, x, y);
}

static bool choice_touch(sl_obj_t *o, int kind, const sl_touch_t *p, int lx, int ly)
{
    (void)p;
    choice_t *ch = ch_of(o);
    if (!ch) {
        return true;
    }
    if (kind == SL_TOUCH_PRESS) {
        ch->press = item_under(ch, lx, ly);
        return !ch->down || apply(o, false, true);
    }
    if (kind != SL_TOUCH_TAP) {
        return true;
    }
    int i = item_under(ch, lx, ly);
    if (i < 0 || i != ch->press) {
        return true;
    }
    sl_anim_flush();
    zval *slot = OBJ_PROP(&o->std, s_off_selected);
    zval v;
    if (!ch->multi) {
        if (Z_TYPE_P(slot) == IS_LONG && Z_LVAL_P(slot) == i) {
            return true;
        }
        zval_ptr_dtor(slot);
        ZVAL_LONG(slot, i);
        ZVAL_LONG(&v, i);
    } else {
        read_selected(o, ch);
        ch->on[i] = !ch->on[i];
        zval_ptr_dtor(slot);
        list_of(ch, slot);
        ZVAL_COPY(&v, slot);
    }
    bool ok = apply(o, false, false) && sl_ctl_notify(o, &v);
    zval_ptr_dtor(&v);
    return ok;
}

static bool choice_write(sl_obj_t *o, const char *name, zval *slot, zval *old)
{
    (void)name;
    choice_t *ch = ch_of(o);
    if (!ch) {
        return true;
    }
    zval norm;
    if (!normal_selected(ZSTR_VAL(o->std.ce->name), ch->n, ch->multi, slot, &norm)) {
        return false;
    }
    zval_ptr_dtor(slot);
    ZVAL_COPY_VALUE(slot, &norm);
    if (zend_is_identical(old, slot)) {
        return true;
    }
    return apply(o, false, false);
}

static void choice_free(sl_obj_t *o)
{
    choice_t *ch = ch_of(o);
    for (int i = 0; i < ch->n; i++) {
        if (ch->labels[i]) {
            zend_string_release(ch->labels[i]);
        }
        if (ch->icons && ch->icons[i]) {
            slate_image_put(ch->icons[i]);
        }
    }
    efree(ch->labels);
    if (ch->icons) {
        efree(ch->icons);
    }
    efree(ch->on);
    efree(ch->box);
    efree(ch->s);
    efree(ch);
}

static const sl_ctl_ops_t choice_ops = {
    .realize = choice_realize,
    .touch = choice_touch,
    .pressed = choice_pressed,
    .write = choice_write,
    .free = choice_free,
};

/* ---- Choice ---- */

/* A non-empty list of strings. */
static bool labels_ok(HashTable *ht)
{
    if (zend_hash_num_elements(ht) == 0 || !zend_array_is_list(ht)) {
        return false;
    }
    zval *v;
    ZEND_HASH_FOREACH_VAL(ht, v) {
        if (Z_TYPE_P(v) != IS_STRING) {
            return false;
        }
    } ZEND_HASH_FOREACH_END();
    return true;
}

PHP_METHOD(SlateChoice, __construct)
{
    zval *x, *y, *selected = NULL;
    HashTable *labels, *icons = NULL;
    zend_object *style = NULL, *size = NULL;
    zend_long w = 0;
    bool multi = false;
    ZEND_PARSE_PARAMETERS_START(3, 9)
        Z_PARAM_NUMBER(x)
        Z_PARAM_NUMBER(y)
        Z_PARAM_ARRAY_HT(labels)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(selected)
        Z_PARAM_OBJ_OF_CLASS(style, sl_choice_style_ce)
        Z_PARAM_LONG(w)
        Z_PARAM_OBJ_OF_CLASS(size, sl_size_ce)
        Z_PARAM_BOOL(multi)
        Z_PARAM_ARRAY_HT(icons)
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
    if (selected && Z_TYPE_P(selected) != IS_LONG && Z_TYPE_P(selected) != IS_ARRAY) {
        zend_argument_type_error(4, "must be of type array|int, %s given", zend_zval_value_name(selected));
        RETURN_THROWS();
    }
    if (!labels_ok(labels)) {
        zend_value_error("%s::$labels must be a non-empty list of strings", cls);
        RETURN_THROWS();
    }
    int n = (int)zend_hash_num_elements(labels);
    zval zsel, def;
    ZVAL_LONG(&def, 0);
    if (!normal_selected(cls, n, multi, selected ? selected : &def, &zsel)) {
        RETURN_THROWS();
    }
    int nicons = icons ? (int)zend_hash_num_elements(icons) : 0;
    int st = ST_SEGMENTED;
    if (style) {
        zval zs;
        ZVAL_OBJ(&zs, style);
        st = sl_enum_case(&zs, style_names, 4);
    }
    bool bad = false;
    if (nicons != 0 && nicons != n) {
        zend_value_error("%s::$icons must be empty or hold one path per label", cls);
        bad = true;
    } else if (w < 0 || w > 32767) {
        zend_value_error("%s::$w must be between 0 and 32767", cls);
        bad = true;
    } else if ((st == ST_LIST || st == ST_NAV) && w == 0) {
        zend_value_error("%s: the List and Nav styles need w greater than 0", cls);
        bad = true;
    } else if (st == ST_NAV && nicons == 0) {
        zend_value_error("%s: the Nav style needs an icon per label", cls);
        bad = true;
    } else if (nicons) {
        zval *v;
        ZEND_HASH_FOREACH_VAL(icons, v) {
            if (Z_TYPE_P(v) != IS_STRING) {
                zend_value_error("%s::$icons must hold absolute paths ending in .sli or .png", cls);
                bad = true;
            } else if (!sl_image_icon_ok(cls, "icons", Z_STR_P(v))) {
                bad = true;
            }
            if (bad) {
                break;
            }
        } ZEND_HASH_FOREACH_END();
    }
    if (bad) {
        zval_ptr_dtor(&zsel);
        RETURN_THROWS();
    }
    obj->handlers->write_property(obj, s_x_name, x, NULL);
    if (!EG(exception)) {
        obj->handlers->write_property(obj, s_y_name, y, NULL);
    }
    if (EG(exception)) {
        zval_ptr_dtor(&zsel);
        RETURN_THROWS();
    }
    zval *slot = OBJ_PROP(obj, s_off_selected);
    zval_ptr_dtor(slot);
    ZVAL_COPY_VALUE(slot, &zsel);

    choice_t *ch = ecalloc(1, sizeof *ch);
    zval zv;
    ZVAL_OBJ(&zv, size ? size : zend_enum_get_case_cstr(sl_size_ce, "Medium"));
    ch->size = (uint8_t)sl_size_of(&zv);
    ch->style = (uint8_t)st;
    ch->multi = multi;
    ch->n = n;
    ch->wgiven = (int)w;
    ch->press = -1;
    ch->labels = ecalloc((size_t)n, sizeof *ch->labels);
    ch->on = ecalloc((size_t)n, sizeof *ch->on);
    ch->box = ecalloc((size_t)n, sizeof *ch->box);
    ch->s = ecalloc((size_t)n, sizeof *ch->s);
    ch->well = ch->bar = -1;
    o->ctl->st = ch;   /* from here choice_free frees what is in it */
    int i = 0;
    zval *v;
    ZEND_HASH_FOREACH_VAL(labels, v) {
        ch->labels[i++] = zend_string_copy(Z_STR_P(v));
    } ZEND_HASH_FOREACH_END();
    if (nicons) {
        ch->icons = ecalloc((size_t)n, sizeof *ch->icons);
        i = 0;
        ZEND_HASH_FOREACH_VAL(icons, v) {
            ch->icons[i] = sl_image_acquire(Z_STR_P(v));
            if (!ch->icons[i]) {
                RETURN_THROWS();
            }
            i++;
        } ZEND_HASH_FOREACH_END();
    }
    if (st == ST_SEGMENTED) {
        ch->well = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    }
    ch->first = o->ctl->nparts;
    for (i = 0; i < n && !EG(exception); i++) {
        sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
        if (nicons) {
            sl_part_add(o, SLATE_EL_IMAGE, -1);
        }
        sl_part_add(o, SLATE_EL_TEXT, -1);
    }
    if (st == ST_NAV && !EG(exception)) {
        ch->bar = sl_part_add(o, SLATE_EL_ROUNDRECT, -1);
    }
    if (EG(exception)) {
        RETURN_THROWS();
    }
    read_selected(o, ch);
    sl_theme_t *th = sl_ctl_theme(o);
    if (th) {
        layout(o, ch, th);
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_choice_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, x, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_MASK(0, y, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO(0, labels, IS_ARRAY, 0)
    ZEND_ARG_TYPE_MASK(0, selected, MAY_BE_LONG | MAY_BE_ARRAY, "0")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, style, Baremetal\\Slate\\ChoiceStyle, 0,
                                         "\\Baremetal\\Slate\\ChoiceStyle::Segmented")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, w, IS_LONG, 0, "0")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, size, Baremetal\\Slate\\Size, 0, "\\Baremetal\\Slate\\Size::Medium")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, multi, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, icons, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_choice_pressed, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, down, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry choice_methods[] = {
    PHP_ME(SlateChoice, __construct, arginfo_choice_ctor, ZEND_ACC_PUBLIC)
    ZEND_MALIAS(SlateControl, pressed, builtinPressed, arginfo_choice_pressed, ZEND_ACC_PROTECTED)
    PHP_FE_END
};

void sl_choice_register(void)
{
    s_x_name = zend_string_init_interned("x", 1, 1);
    s_y_name = zend_string_init_interned("y", 1, 1);
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Choice", choice_methods);
    sl_choice_ce = zend_register_internal_class_ex(&ce, sl_control_ce);   /* not final: an app may extend it */
    sl_choice_ce->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    zval dv;
    ZVAL_UNDEF(&dv);
    zend_property_info *pi = zend_declare_typed_property(
        sl_choice_ce, zend_string_init_interned("selected", sizeof("selected") - 1, 1), &dv, ZEND_ACC_PUBLIC, NULL,
        (zend_type)ZEND_TYPE_INIT_MASK(MAY_BE_LONG | MAY_BE_ARRAY));
    s_off_selected = pi->offset;
    sl_ctl_class(sl_choice_ce, SL_CHOICE, &choice_ops);
}

#endif
