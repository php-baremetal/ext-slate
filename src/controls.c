/* controls.c - the pure rules of Slate's controls (controls.h). */
#include "slate_config.h"
#include "controls.h"

#include <math.h>

#if SLATE_HAS_CONTROLS

static int clamp_pct(int pct)
{
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

uint32_t slate_ctl_mix(uint32_t rgb, uint32_t to, int pct)
{
    pct = clamp_pct(pct);
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        uint32_t c = (rgb >> s) & 0xFFu, t = (to >> s) & 0xFFu;
        out |= ((c * (uint32_t)(100 - pct) + t * (uint32_t)pct + 50u) / 100u) << s;
    }
    return out;
}

uint32_t slate_ctl_alpha(uint32_t rgb, int pct)
{
    uint32_t a = (255u * (uint32_t)clamp_pct(pct) + 50u) / 100u;
    return (a << 24) | (rgb & 0xFFFFFFu);
}

uint32_t slate_ctl_over(uint32_t top, uint32_t bottom)
{
    uint32_t at = top >> 24, ab = bottom >> 24;
    uint32_t num = at * 255u + ab * (255u - at);   /* a_o, times 255 */
    if (num == 0) {
        return 0;
    }
    uint32_t out = ((num + 127u) / 255u) << 24;
    for (int s = 0; s <= 16; s += 8) {
        uint32_t ct = (top >> s) & 0xFFu, cb = (bottom >> s) & 0xFFu;
        out |= ((ct * at * 255u + cb * ab * (255u - at) + num / 2) / num) << s;
    }
    return out;
}

uint32_t slate_ctl_pressed(uint32_t fill, uint32_t text)
{
    if ((fill >> 24) == 0xFFu) {
        return 0xFF000000u | slate_ctl_mix(fill & 0xFFFFFFu, 0xFFFFFF, SLATE_CTL_PRESS_PCT);
    }
    return slate_ctl_over(slate_ctl_alpha(text, 6), fill);
}

int slate_ctl_pad(slate_ctl_size_t s)
{
    return s == SLATE_CTL_SMALL ? 16 : s == SLATE_CTL_LARGE ? 24 : 20;
}

int slate_ctl_button_w(int text_w, slate_ctl_size_t s, bool icon, int h)
{
    if (text_w <= 0 && icon) {
        return h;
    }
    return (text_w > 0 ? text_w : 0) + 2 * slate_ctl_pad(s) + (icon ? SLATE_CTL_ICON + SLATE_CTL_ICON_GAP : 0);
}

slate_ctl_toggle_t slate_ctl_toggle_dims(slate_ctl_size_t s)
{
    slate_ctl_toggle_t d = { 62, 36, 28, 4 };
    if (s == SLATE_CTL_SMALL) {
        d.w = 52;
        d.h = 30;
        d.knob = 24;
    } else if (s == SLATE_CTL_LARGE) {
        d.w = 72;
        d.h = 42;
        d.knob = 34;
    }
    return d;
}

int slate_ctl_toggle_knob_x(const slate_ctl_toggle_t *d, bool on)
{
    return on ? d->w - d->margin - d->knob : d->margin;
}

int slate_ctl_slider_knob(slate_ctl_size_t s)
{
    return s == SLATE_CTL_SMALL ? 24 : s == SLATE_CTL_LARGE ? 34 : 30;
}

/* x is a whole number (at least 1 in size, or zero when zero is allowed), within a relative error of 1e-9 and at
   most 1e-6 off (a large x with a fraction, 1e15 + 0.5, is not whole). */
static bool whole(double x, bool zero)
{
    double n = nearbyint(x);
    if (n == 0) {
        return zero && x == 0;
    }
    double e = fabs(x - n);
    return e <= 1e-9 * fabs(x) && e <= 1e-6;
}

double slate_ctl_snap(double v, double min, double max, double step)
{
    if (!(v > min)) {   /* NaN too */
        return min;
    }
    double last = floor((max - min) / step + 1e-9);   /* the last step at or below max */
    double k = floor((v - min) / step + 0.5);
    if (k > last) {
        k = last;
    }
    /* to the decimals of step and min (at most 9): min 0, step 0.1, k 3 gives 0.3, not 0.30000000000000004; r as
       it is when they have more (1e-10, 1/3) or r * 10^d leaves the exact doubles (2^53) */
    double r = min + k * step, p = 1;
    for (int d = 0; !whole(step * p, false) || !whole(min * p, true); d++) {
        if (d == 9) {
            return r;
        }
        p *= 10;
    }
    if (!(fabs(r * p) < 9007199254740992.0)) {
        return r;
    }
    return nearbyint(r * p) / p;
}

double slate_ctl_slider_value(double lx, int w, int knob, double min, double max, double step)
{
    if (w <= knob) {
        return min;
    }
    double t = (lx - knob / 2.0) / (double)(w - knob);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return slate_ctl_snap(min + t * (max - min), min, max, step);
}

double slate_ctl_slider_x(double value, int w, int knob, double min, double max)
{
    if (w <= knob || !(max > min)) {
        return knob / 2.0;
    }
    double t = (value - min) / (max - min);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return knob / 2.0 + t * (double)(w - knob);
}

/* ---- Choice ---- */

static int16_t i16(int v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static slate_ctl_box_t box(int x, int y, int w, int h)
{
    slate_ctl_box_t b = { i16(x), i16(y), i16(w), i16(h) };
    return b;
}

/* n equal parts of total from x0 (the remainder to the first ones), each h high at y. */
static void split(int x0, int y, int total, int n, int h, slate_ctl_box_t *out)
{
    if (total < 0) {
        total = 0;
    }
    int each = total / n, rest = total % n, x = x0;
    for (int i = 0; i < n; i++) {
        int w = each + (i < rest ? 1 : 0);
        out[i] = box(x, y, w, h);
        x += w;
    }
}

int slate_ctl_segments(int w, int n, const int *item_w, int h, slate_ctl_box_t *out)
{
    if (n <= 0) {
        return w > 0 ? w : 8;
    }
    if (w <= 0) {
        int widest = 0;
        for (int i = 0; i < n; i++) {
            widest = item_w[i] > widest ? item_w[i] : widest;
        }
        w = n * widest + 8;
    }
    split(4, 4, w - 8, n, h - 8, out);
    return w;
}

int slate_ctl_chips(int w, int n, const int *item_w, int h, slate_ctl_box_t *out)
{
    int x = 0, y = 0;
    for (int i = 0; i < n; i++) {
        if (w > 0 && x > 0 && x + item_w[i] > w) {
            x = 0;
            y += h + 8;
        }
        out[i] = box(x, y, item_w[i], h);
        x += item_w[i] + 8;
    }
    return y + h;
}

void slate_ctl_rows(int w, int n, int h, slate_ctl_box_t *out)
{
    for (int i = 0; i < n; i++) {
        out[i] = box(0, i * h, w, h);
    }
}

void slate_ctl_nav(int w, int n, int h, slate_ctl_box_t *out)
{
    if (n > 0) {
        split(0, 0, w, n, h, out);
    }
}

slate_ctl_box_t slate_ctl_nav_bar(const slate_ctl_box_t *part)
{
    int inset = (part->w * 32 + 50) / 100;
    return box(part->x + inset, 0, part->w - 2 * inset, 3);
}

int slate_ctl_item_at(const slate_ctl_box_t *items, int n, int x, int y)
{
    for (int i = 0; i < n; i++) {
        const slate_ctl_box_t *b = &items[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            return i;
        }
    }
    return -1;
}

slate_ctl_row_t slate_ctl_row(int w, int label_w, int label_lh, int sub_w, int sub_lh, int value_w, int ctl_w,
                              int ctl_h, bool stretch)
{
    slate_ctl_row_t r;
    bool sub = sub_lh > 0;
    r.h = sub ? 72 : 60;
    if (ctl_h + 16 > r.h) {
        r.h = ctl_h + 16;
    }
    int block = sub ? label_lh + 2 + sub_lh : label_lh;
    r.label_y = (r.h - block) / 2;
    r.sub_y = sub ? r.label_y + label_lh + 2 : r.label_y;
    r.value_x = w;
    int right = w - (value_w > 0 ? value_w + 12 : 0);
    r.ctl_x = right - ctl_w;
    r.ctl_y = (r.h - ctl_h) / 2;
    r.slider_x = 0;
    r.slider_w = 0;
    if (stretch) {
        int lw = label_w > sub_w ? label_w : sub_w;
        int cap = w * 40 / 100;
        r.slider_x = (lw < cap ? lw : cap) + 12;
        r.slider_w = right - r.slider_x;
        if (r.slider_w < 0) {
            r.slider_w = 0;
        }
    }
    return r;
}

/* A section of size (0: absent, at y) below what is there: 12 under the previous one present. Its top. */
static int section(int *y, bool *any, int size)
{
    if (size <= 0) {
        return *y;
    }
    int top = *y + (*any ? SLATE_CTL_DIALOG_GAP : 0);
    *y = top + size;
    *any = true;
    return top;
}

slate_ctl_panel_t slate_ctl_panel(int w, int sw, int sh, int title_lh, int lines, int line_h, int content_h,
                                  int n_buttons, const int *btn_h, bool stack)
{
    slate_ctl_panel_t p;
    p.w = w < sw - 32 ? w : sw - 32;
    if (p.w < 1) {
        p.w = 1;
    }
    p.x = (sw - p.w) / 2;
    int y = SLATE_CTL_DIALOG_PAD;
    bool any = false;
    p.title_y = section(&y, &any, title_lh);
    p.text_y = section(&y, &any, lines > 0 ? lines * line_h : 0);
    p.content_y = section(&y, &any, content_h);
    int bh = 0;
    for (int i = 0; i < n_buttons; i++) {
        if (stack) {
            bh += btn_h[i] + (i > 0 ? SLATE_CTL_DIALOG_GAP : 0);
        } else if (btn_h[i] > bh) {
            bh = btn_h[i];
        }
    }
    p.buttons_y = section(&y, &any, n_buttons > 0 ? bh : 0);
    p.h = y + SLATE_CTL_DIALOG_PAD;
    if (p.h > sh - 32) {
        p.h = sh - 32;
    }
    if (p.h < 1) {
        p.h = 1;
    }
    p.y = (sh - p.h) / 2;
    return p;
}

void slate_ctl_dialog_buttons(int inner_w, int n, const int *btn_h, bool stack, slate_ctl_box_t *out)
{
    if (n <= 0) {
        return;
    }
    if (stack) {
        int y = 0;
        for (int i = 0; i < n; i++) {
            out[i] = box(0, y, inner_w, btn_h[i]);
            y += btn_h[i] + SLATE_CTL_DIALOG_GAP;
        }
        return;
    }
    int total = inner_w - (n - 1) * SLATE_CTL_DIALOG_GAP;
    if (total < 0) {
        total = 0;
    }
    int each = total / n, rest = total % n, x = 0;
    for (int i = 0; i < n; i++) {
        int bw = each + (i < rest ? 1 : 0);
        out[i] = box(x, 0, bw, btn_h[i]);
        x += bw + SLATE_CTL_DIALOG_GAP;
    }
}

slate_ctl_box_t slate_ctl_toast(int sw, int sh, int text_w)
{
    int w = SLATE_CTL_TOAST_PAD + SLATE_CTL_TOAST_DOT + SLATE_CTL_TOAST_GAP + text_w + SLATE_CTL_TOAST_PAD;
    return box((sw - w) / 2, sh - sh / 6 - SLATE_CTL_TOAST_H, w, SLATE_CTL_TOAST_H);
}

float slate_ctl_ring_to(float v)
{
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return -90.0f + v * 360.0f;
}

slate_ctl_box_t slate_ctl_step_row(int i, int w)
{
    return box(0, i * (SLATE_CTL_STEP_H + SLATE_CTL_STEP_GAP), w, SLATE_CTL_STEP_H);
}

void slate_ctl_check(float pts[6])
{
    static const float p[6] = { 18, 24, 22, 28, 30, 20 };
    for (int i = 0; i < 6; i++) {
        pts[i] = p[i];
    }
}

#endif
