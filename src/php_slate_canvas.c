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
 * Canvas: an element with pixels of its own. The script draws with the primitives of ext-display (clear, pixel,
 * line, rect, fillRect, circle, fillCircle, text) into a buffer on the PHP side (RGB565, opaque, colours given
 * as 0xRRGGBB); dirty() sends the rectangle changed since the last call to the compositor (one PIXELS command),
 * which shows it at the next frame. Drawing is immediate-mode and costs nothing in the compositor until dirty().
 */
#include <string.h>

#include "php.h"
#include "zend_exceptions.h"

#include "display_text.h"
#include "php_slate.h"

static uint16_t rgb565(zend_long c)
{
    uint32_t v = (uint32_t)c;
    return (uint16_t)((((v >> 19) & 0x1F) << 11) | (((v >> 10) & 0x3F) << 5) | ((v >> 3) & 0x1F));
}

static bool color_arg(uint32_t n, zend_long c)
{
    if (c < 0 || c > 0xFFFFFF) {
        zend_argument_value_error(n, "must be a color 0xRRGGBB (0 to 0xFFFFFF)");
        return false;
    }
    return true;
}

static void mark(sl_obj_t *o, int x0, int y0, int x1, int y1)
{
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > o->cw ? o->cw : x1;
    y1 = y1 > o->ch ? o->ch : y1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (o->dx0 >= o->dx1) {
        o->dx0 = x0;
        o->dy0 = y0;
        o->dx1 = x1;
        o->dy1 = y1;
        return;
    }
    o->dx0 = x0 < o->dx0 ? x0 : o->dx0;
    o->dy0 = y0 < o->dy0 ? y0 : o->dy0;
    o->dx1 = x1 > o->dx1 ? x1 : o->dx1;
    o->dy1 = y1 > o->dy1 ? y1 : o->dy1;
}

static void plot(sl_obj_t *o, int64_t x, int64_t y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < o->cw && y < o->ch) {
        o->px[(size_t)y * o->cw + (size_t)x] = c;
    }
}

static int64_t clamp64(int64_t v)
{
    return v < -(1 << 20) ? -(1 << 20) : v > (1 << 20) ? (1 << 20) : v;   /* no overflow in x + w */
}

static void fill(sl_obj_t *o, int64_t x, int64_t y, int64_t w, int64_t h, uint16_t c)
{
    x = clamp64(x);
    y = clamp64(y);
    w = clamp64(w);
    h = clamp64(h);
    int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int64_t x1 = x + w > o->cw ? o->cw : x + w, y1 = y + h > o->ch ? o->ch : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (int64_t j = y0; j < y1; j++) {
        uint16_t *row = o->px + (size_t)j * o->cw;
        for (int64_t i = x0; i < x1; i++) {
            row[i] = c;
        }
    }
    mark(o, (int)x0, (int)y0, (int)x1, (int)y1);
}

static void hline(sl_obj_t *o, int64_t x, int64_t y, int64_t w, uint16_t c)
{
    fill(o, x, y, w, 1, c);
}

static int clampi(zend_long v)
{
    return v < -65536 ? -65536 : v > 65536 ? 65536 : (int)v;
}

bool sl_canvas_resize(sl_obj_t *o, int w, int h)
{
    if (o->px) {
        efree(o->px);
        o->px = NULL;
    }
    o->cw = w > 0 && h > 0 ? w : 0;
    o->ch = w > 0 && h > 0 ? h : 0;
    o->dx0 = o->dy0 = o->dx1 = o->dy1 = 0;
    if (o->cw) {
        o->px = ecalloc((size_t)o->cw * (size_t)o->ch, sizeof(uint16_t));
    }
    return true;
}

bool sl_canvas_send(sl_obj_t *o, int x0, int y0, int x1, int y1)
{
    if (!o->px || x0 >= x1 || y0 >= y1) {
        return true;
    }
    int w = x1 - x0;
    const uint16_t *data;
    uint16_t *tmp = NULL;
    if (x0 == 0 && x1 == o->cw) {
        data = o->px + (size_t)y0 * o->cw;   /* full rows: contiguous already */
    } else {
        tmp = emalloc((size_t)w * (size_t)(y1 - y0) * sizeof(uint16_t));
        for (int j = y0; j < y1; j++) {
            memcpy(tmp + (size_t)(j - y0) * w, o->px + (size_t)j * o->cw + x0, (size_t)w * sizeof(uint16_t));
        }
        data = tmp;
    }
    slate_cmd_t c = { .kind = SLATE_CMD_PIXELS, .id = o->id, .r = { x0, y0, x1, y1 }, .data = data };
    bool ok = sl_push(&c);
    if (tmp) {
        efree(tmp);
    }
    return ok;
}

static sl_obj_t *this_canvas(zval *this_ptr)
{
    return sl_obj(Z_OBJ_P(this_ptr));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_canvas_clear, 0, 0, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, color, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

PHP_METHOD(SlateCanvas, clear)
{
    zend_long color = 0;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(color)
    ZEND_PARSE_PARAMETERS_END();
    if (!color_arg(1, color)) {
        RETURN_THROWS();
    }
    sl_obj_t *o = this_canvas(ZEND_THIS);
    fill(o, 0, 0, o->cw, o->ch, rgb565(color));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_canvas_pixel, 0, 3, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(SlateCanvas, pixel)
{
    zend_long x, y, color;
    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(color)
    ZEND_PARSE_PARAMETERS_END();
    if (!color_arg(3, color)) {
        RETURN_THROWS();
    }
    fill(this_canvas(ZEND_THIS), x, y, 1, 1, rgb565(color));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_canvas_line, 0, 5, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, x0, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y0, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, x1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(SlateCanvas, line)
{
    zend_long lx0, ly0, lx1, ly1, color;
    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_LONG(lx0)
        Z_PARAM_LONG(ly0)
        Z_PARAM_LONG(lx1)
        Z_PARAM_LONG(ly1)
        Z_PARAM_LONG(color)
    ZEND_PARSE_PARAMETERS_END();
    if (!color_arg(5, color)) {
        RETURN_THROWS();
    }
    sl_obj_t *o = this_canvas(ZEND_THIS);
    uint16_t c = rgb565(color);
    int x0 = clampi(lx0), y0 = clampi(ly0), x1 = clampi(lx1), y1 = clampi(ly1);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    mark(o, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, (x0 > x1 ? x0 : x1) + 1, (y0 > y1 ? y0 : y1) + 1);
    for (;;) {
        plot(o, x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_canvas_rect, 0, 5, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, h, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

static bool rect_args(INTERNAL_FUNCTION_PARAMETERS, zend_long *x, zend_long *y, zend_long *w, zend_long *h,
                      zend_long *color)
{
    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_LONG(*x)
        Z_PARAM_LONG(*y)
        Z_PARAM_LONG(*w)
        Z_PARAM_LONG(*h)
        Z_PARAM_LONG(*color)
    ZEND_PARSE_PARAMETERS_END_EX(return false);
    (void)return_value;
    return color_arg(5, *color);
}

PHP_METHOD(SlateCanvas, rect)
{
    zend_long x, y, w, h, color;
    if (!rect_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, &x, &y, &w, &h, &color)) {
        RETURN_THROWS();
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    sl_obj_t *o = this_canvas(ZEND_THIS);
    uint16_t c = rgb565(color);
    hline(o, x, y, w, c);
    hline(o, x, (int64_t)y + h - 1, w, c);
    fill(o, x, y, 1, h, c);
    fill(o, (int64_t)x + w - 1, y, 1, h, c);
}

PHP_METHOD(SlateCanvas, fillRect)
{
    zend_long x, y, w, h, color;
    if (!rect_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, &x, &y, &w, &h, &color)) {
        RETURN_THROWS();
    }
    if (w > 0 && h > 0) {
        fill(this_canvas(ZEND_THIS), x, y, w, h, rgb565(color));
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_canvas_circle, 0, 4, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, cx, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, cy, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, r, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Midpoint circle: body runs for each octant point (x, y), x <= y. */
static void circle(sl_obj_t *o, int cx, int cy, int r, uint16_t c, bool filled)
{
    int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
    for (;;) {
        if (filled) {
            hline(o, cx - x, cy + y, 2 * x + 1, c);
            hline(o, cx - x, cy - y, 2 * x + 1, c);
            hline(o, cx - y, cy + x, 2 * y + 1, c);
            hline(o, cx - y, cy - x, 2 * y + 1, c);
        } else {
            plot(o, cx + x, cy + y, c); plot(o, cx - x, cy + y, c);
            plot(o, cx + x, cy - y, c); plot(o, cx - x, cy - y, c);
            plot(o, cx + y, cy + x, c); plot(o, cx - y, cy + x, c);
            plot(o, cx + y, cy - x, c); plot(o, cx - y, cy - x, c);
        }
        if (x >= y) {
            break;
        }
        if (f >= 0) {
            y--;
            ddy += 2;
            f += ddy;
        }
        x++;
        ddx += 2;
        f += ddx;
    }
    mark(o, cx - r, cy - r, cx + r + 1, cy + r + 1);
}

static bool circle_args(INTERNAL_FUNCTION_PARAMETERS, zend_long *cx, zend_long *cy, zend_long *r, zend_long *color)
{
    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_LONG(*cx)
        Z_PARAM_LONG(*cy)
        Z_PARAM_LONG(*r)
        Z_PARAM_LONG(*color)
    ZEND_PARSE_PARAMETERS_END_EX(return false);
    (void)return_value;
    return color_arg(4, *color);
}

PHP_METHOD(SlateCanvas, circle)
{
    zend_long cx, cy, r, color;
    if (!circle_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, &cx, &cy, &r, &color)) {
        RETURN_THROWS();
    }
    if (r >= 0) {
        circle(this_canvas(ZEND_THIS), clampi(cx), clampi(cy), clampi(r), rgb565(color), false);
    }
}

PHP_METHOD(SlateCanvas, fillCircle)
{
    zend_long cx, cy, r, color;
    if (!circle_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, &cx, &cy, &r, &color)) {
        RETURN_THROWS();
    }
    if (r >= 0) {
        circle(this_canvas(ZEND_THIS), clampi(cx), clampi(cy), clampi(r), rgb565(color), true);
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_canvas_text, 0, 4, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bg, IS_LONG, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, font, IS_STRING, 0, "'default'")
ZEND_END_ARG_INFO()

static uint16_t blend(uint16_t fg, uint16_t bg, unsigned v, unsigned max)
{
    unsigned r = (((fg >> 11) & 0x1F) * v + ((bg >> 11) & 0x1F) * (max - v) + max / 2) / max;
    unsigned g = (((fg >> 5) & 0x3F) * v + ((bg >> 5) & 0x3F) * (max - v) + max / 2) / max;
    unsigned b = ((fg & 0x1F) * v + (bg & 0x1F) * (max - v) + max / 2) / max;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* text(x, y, s, color, bg = null, font = 'default'): one line, y the top of the line; returns its width. Edge
   levels of an anti-aliased font are blended with what is under them (or with bg). */
PHP_METHOD(SlateCanvas, text)
{
    zend_long x, y, color, bg = 0;
    bool bg_null = true;
    zend_string *s, *font = NULL;
    ZEND_PARSE_PARAMETERS_START(4, 6)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_STR(s)
        Z_PARAM_LONG(color)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(bg, bg_null)
        Z_PARAM_STR(font)
    ZEND_PARSE_PARAMETERS_END();
    if (!color_arg(4, color) || (!bg_null && !color_arg(5, bg))) {
        RETURN_THROWS();
    }
    const char *fname = font ? ZSTR_VAL(font) : "default";
    size_t flen = font ? ZSTR_LEN(font) : 7;
    const struct display_font_entry *e = NULL;
    for (size_t i = 0; i < display_fonts_count; i++) {
        const struct display_font_entry *f = &display_fonts[i];
        if (f->name && strlen(f->name) == flen && memcmp(f->name, fname, flen) == 0) {
            e = f;
            break;
        }
    }
    display_text_font_t f;
    if (!e || !display_text_font_entry(&f, e)) {
        zend_argument_value_error(6, "must be a font of the firmware, '%s' is not", fname);
        RETURN_THROWS();
    }
    sl_obj_t *o = this_canvas(ZEND_THIS);
    uint16_t fg = rgb565(color), b565 = bg_null ? 0 : rgb565(bg);
    const char *p = ZSTR_VAL(s), *end = p + ZSTR_LEN(s);
    int64_t cx = clampi(x);
    int top = clampi(y), base = top + f.ascent;
    unsigned max = (1u << f.bpp) - 1;
    while (p < end) {
        const GFXglyph *g = display_text_glyph(&f, display_text_utf8(&p, end));
        int adv = display_text_advance(&f, g);
        if (cx < o->cw) {
            if (!bg_null) {
                fill(o, cx, top, adv, f.line_height, b565);
            }
            if (g) {
                const uint8_t *bits = f.bitmap + g->bitmapOffset;
                int64_t gx = cx + g->xOffset, gy = (int64_t)base + g->yOffset;
                size_t bit = 0;
                for (int j = 0; j < g->height; j++) {
                    for (int i = 0; i < g->width; i++, bit += f.bpp) {
                        unsigned v = (bits[bit / 8] >> (8 - f.bpp - bit % 8)) & max;
                        int64_t px = gx + i, py = gy + j;
                        if (v && px >= 0 && py >= 0 && px < o->cw && py < o->ch) {
                            uint16_t *d = &o->px[(size_t)py * o->cw + (size_t)px];
                            *d = v == max ? fg : blend(fg, *d, v, max);
                        }
                    }
                }
                mark(o, (int)gx, (int)gy, (int)(gx + g->width), (int)(gy + g->height));
            } else {   /* a codepoint the font lacks: a box, as ext-display draws it */
                hline(o, cx, top + 1, adv - 1, fg);
                hline(o, cx, top + f.line_height - 2, adv - 1, fg);
                fill(o, cx, top + 1, 1, f.line_height - 2, fg);
                fill(o, cx + adv - 2, top + 1, 1, f.line_height - 2, fg);
            }
        }
        cx += adv;
    }
    zend_long w = (zend_long)(cx - clampi(x));
    RETURN_LONG(w);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_canvas_dirty, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

/* Sends what changed since the last dirty() (nothing when nothing changed, or before the canvas is shown: it
   goes whole then). */
PHP_METHOD(SlateCanvas, dirty)
{
    ZEND_PARSE_PARAMETERS_NONE();
    sl_obj_t *o = this_canvas(ZEND_THIS);
    int x0 = o->dx0, y0 = o->dy0, x1 = o->dx1, y1 = o->dy1;
    o->dx0 = o->dy0 = o->dx1 = o->dy1 = 0;
    if (x0 >= x1 || !sl_g.engine || o->gen != sl_g.gen) {
        return;
    }
    if (!sl_canvas_send(o, x0, y0, x1, y1)) {
        RETURN_THROWS();
    }
}

PHP_METHOD(SlateCanvas, __construct);   /* php_slate_elements.c */

ZEND_BEGIN_ARG_INFO_EX(arginfo_canvas_ctor, 0, 0, 4)
    ZEND_ARG_TYPE_MASK(0, x, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_MASK(0, y, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, h, IS_LONG, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry canvas_methods[] = {
    PHP_ME(SlateCanvas, __construct, arginfo_canvas_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, clear, arginfo_canvas_clear, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, pixel, arginfo_canvas_pixel, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, line, arginfo_canvas_line, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, rect, arginfo_canvas_rect, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, fillRect, arginfo_canvas_rect, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, circle, arginfo_canvas_circle, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, fillCircle, arginfo_canvas_circle, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, text, arginfo_canvas_text, ZEND_ACC_PUBLIC)
    PHP_ME(SlateCanvas, dirty, arginfo_canvas_dirty, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const zend_function_entry *sl_canvas_method_table(void)
{
    return canvas_methods;
}
