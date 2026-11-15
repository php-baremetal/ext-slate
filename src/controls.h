/* controls.h - the pure rules of Slate's controls (php_slate_control.c, php_slate_ctl_*.c): colour mixing and alpha,
   paddings, sizes, the Toggle's measures, the Slider's maths, the Choice's layouts, the SettingRow's, the Dialog's, the
   toast's, the progress controls' and the StepList's. Pure C, no ESP-IDF and no PHP.

   Colours here are 0xRRGGBB (no alpha byte) unless said otherwise. The pressed look of a control mixes its fill
   SLATE_CTL_PRESS_PCT percent toward white in SLATE_CTL_PRESS_MS; a control that is not enabled is drawn at
   SLATE_CTL_DISABLED times its opacity. */
#ifndef SLATE_CONTROLS_H
#define SLATE_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { SLATE_CTL_SMALL, SLATE_CTL_MEDIUM, SLATE_CTL_LARGE } slate_ctl_size_t;

#define SLATE_CTL_ICON 20        /* icon side in a Button, a segment, a chip, a List row */
#define SLATE_CTL_ICON_GAP 8     /* between an icon and its text */
#define SLATE_CTL_PRESS_PCT 12   /* the pressed look: toward white */
#define SLATE_CTL_PRESS_MS 80
#define SLATE_CTL_DISABLED 0.4f

/* rgb (0xRRGGBB) toward to by pct percent (0..100), each channel (c * (100 - pct) + t * pct + 50) / 100; 0xRRGGBB. */
uint32_t slate_ctl_mix(uint32_t rgb, uint32_t to, int pct);
/* 0xAARRGGBB of rgb at pct percent (0..100): AA = (255 * pct + 50) / 100. */
uint32_t slate_ctl_alpha(uint32_t rgb, int pct);
/* top (0xAARRGGBB) laid over bottom (0xAARRGGBB), Porter-Duff over, as one 0xAARRGGBB that does not depend on what
   is behind: a_o = a_t + a_b (1 - a_t), C_o = (C_t a_t + C_b a_b (1 - a_t)) / a_o (rounded; both alpha 0: 0). Over a
   bottom at alpha 0 it is top exactly. */
uint32_t slate_ctl_over(uint32_t top, uint32_t bottom);
/* The pressed look of a fill (0xAARRGGBB): opaque, SLATE_CTL_PRESS_PCT toward white; otherwise text (0xRRGGBB) at
   6 % laid over it (slate_ctl_over), so a transparent fill gets text at 6 % (a Ghost Button) and a translucent one
   keeps its hue. */
uint32_t slate_ctl_pressed(uint32_t fill, uint32_t text);
/* Horizontal padding of a Button, a segment or a chip: 16, 20, 24 for Small, Medium, Large. */
int slate_ctl_pad(slate_ctl_size_t s);
/* Width of a Button laid out by its content (w = 0): text_w + 2 paddings, + SLATE_CTL_ICON + SLATE_CTL_ICON_GAP with
   an icon; no text (text_w 0) and an icon: h (a square). */
int slate_ctl_button_w(int text_w, slate_ctl_size_t s, bool icon, int h);

/* Toggle: the track w x h (radius h / 2), the knob's diameter and its margin from the track's ends:
   52 x 30 / 24, 62 x 36 / 28, 72 x 42 / 34 for Small, Medium, Large; margin 4. */
#define SLATE_CTL_TOGGLE_MS 250
typedef struct { int w, h, knob, margin; } slate_ctl_toggle_t;
slate_ctl_toggle_t slate_ctl_toggle_dims(slate_ctl_size_t s);
/* The knob's left edge: off, the margin; on, w - margin - knob. */
int slate_ctl_toggle_knob_x(const slate_ctl_toggle_t *d, bool on);

/* Slider: the knob's diameter, 24, 30, 34 for Small, Medium, Large; the track 8 high (radius 4), the knob's border 5;
   a drag is the slider's past SLATE_CTL_SLIDER_SLOP px from the Press. */
#define SLATE_CTL_SLIDER_TRACK 8
#define SLATE_CTL_SLIDER_BORDER 5
#define SLATE_CTL_SLIDER_SLOP 8
int slate_ctl_slider_knob(slate_ctl_size_t s);
/* v on the step grid from min (min + k * step), within min..max (the last step at or below max), rounded to the
   decimals of step and min (at most 9), so step 0.1 gives 0.3 and not 0.30000000000000004. */
double slate_ctl_snap(double v, double min, double max, double step);
/* The value under local x: the knob's centre runs from knob / 2 to w - knob / 2 (w <= knob: min), then snapped. */
double slate_ctl_slider_value(double lx, int w, int knob, double min, double max, double step);
/* The knob's centre for value (the inverse, unsnapped; w <= knob: knob / 2). */
double slate_ctl_slider_x(double value, int w, int knob, double min, double max);

/* Choice: the items of the four styles, in the control's local coordinates; colours and pressed look ease in
   SLATE_CTL_CHOICE_MS. */
#define SLATE_CTL_CHOICE_MS 200
#define SLATE_CTL_NAV_ICON 24    /* icon side in a Nav part */
typedef struct { int16_t x, y, w, h; } slate_ctl_box_t;
/* Segmented: container (0, 0, total, h) with padding 4; n equal segments of (w - 8) / n (the remainder to the
   first ones), or with w == 0 each max(item_w) wide; item_w = text + 2 pad (+ 28 with icons). Returns the width. */
int slate_ctl_segments(int w, int n, const int *item_w, int h, slate_ctl_box_t *out);
/* Chips: item_w each, gap 8; w == 0: one row; w > 0: a chip that does not fit goes to the next row (one wider than
   w alone in its row); rows h + 8 apart. Returns the height. */
int slate_ctl_chips(int w, int n, const int *item_w, int h, slate_ctl_box_t *out);
/* List: rows of w x h, one under the other, no gap. Nav: n equal parts of w (remainder to the first ones), h. */
void slate_ctl_rows(int w, int n, int h, slate_ctl_box_t *out);
void slate_ctl_nav(int w, int n, int h, slate_ctl_box_t *out);
/* Nav: the accent bar of a part w wide: inset 32 % each side, 3 high at the top */
slate_ctl_box_t slate_ctl_nav_bar(const slate_ctl_box_t *part);
/* The item under local (x, y), -1: none (a gap) */
int slate_ctl_item_at(const slate_ctl_box_t *items, int n, int x, int y);

/* SettingRow: the label on the left (with the subtitle under it), the value right-aligned at w, the control left of
   the value (gap 12), every part centred in the height. w; the label's, subtitle's and value's widths and line heights
   (sub_lh 0: no subtitle; value_w 0: no value); the control's w x h (0 x 0: none); stretch: the control is a Slider
   with w 0, which gets slider_x and slider_w (the space between the label, at most 40 % of w, and the value, gap 12).
   h = max(sub ? 72 : 60, ctl_h + 16); with a subtitle the block label_lh + 2 + sub_lh is centred. */
typedef struct { int h, label_y, sub_y, ctl_x, ctl_y, value_x, slider_x, slider_w; } slate_ctl_row_t;
slate_ctl_row_t slate_ctl_row(int w, int label_w, int label_lh, int sub_w, int sub_lh, int value_w, int ctl_w,
                              int ctl_h, bool stretch);

/* Dialog: the panel, W = min(w, sw - 32) centred on the sw x sh screen. From the top, padding 26, then the title
   (title_lh 0: none), the text (lines of line_h), the content (content_h 0: none) and the buttons (n_buttons of
   btn_h: in a row, the tallest; with stack, a column), 12 apart; padding 26 at the bottom. h cut to sh - 32. x, y: its
   top-left on the screen; the *_y: from the panel's top. */
#define SLATE_CTL_DIALOG_PAD 26
#define SLATE_CTL_DIALOG_GAP 12
#define SLATE_CTL_DIALOG_MS 300
#define SLATE_CTL_DIALOG_RISE 14
typedef struct { int x, y, w, h, title_y, text_y, content_y, buttons_y; } slate_ctl_panel_t;
slate_ctl_panel_t slate_ctl_panel(int w, int sw, int sh, int title_lh, int lines, int line_h, int content_h,
                                  int n_buttons, const int *btn_h, bool stack);
/* The buttons in the panel's inner width: a row of equal widths (the remainder to the first ones), or with stack a
   column at full width; gap 12; each its own btn_h; positions relative to (26, buttons_y). */
void slate_ctl_dialog_buttons(int inner_w, int n, const int *btn_h, bool stack, slate_ctl_box_t *out);

/* The toast: a pill 48 high, padding 20, a dot of 8, gap 10, then the text (text_w); centred, its bottom edge at
   sh - sh / 6. */
#define SLATE_CTL_TOAST_H 48
#define SLATE_CTL_TOAST_PAD 20
#define SLATE_CTL_TOAST_DOT 8
#define SLATE_CTL_TOAST_GAP 10
slate_ctl_box_t slate_ctl_toast(int sw, int sh, int text_w);

/* ProgressRing, ProgressBar, Spinner: their value and colour changes ease in SLATE_CTL_PROGRESS_MS; a turn of the
   Spinner and of a spinning ring is SLATE_CTL_SPIN_MS (Linear, repeated forever), its arc 25 % (90 degrees). */
#define SLATE_CTL_PROGRESS_MS 200
#define SLATE_CTL_SPIN_MS 900
#define SLATE_CTL_SPINNER_WIDTH 3
/* ProgressRing: the end angle of value v (0..1): -90 + v * 360 */
float slate_ctl_ring_to(float v);

/* StepList: row i at y = i * 56 (48 high, gap 8), circle centre (24, 24) radius 12, text at x 48 */
#define SLATE_CTL_STEP_H 48
#define SLATE_CTL_STEP_GAP 8
#define SLATE_CTL_STEP_RADIUS 14
#define SLATE_CTL_STEP_DOT 12
#define SLATE_CTL_STEP_DOT_X 24
#define SLATE_CTL_STEP_TEXT_X 48
#define SLATE_CTL_STEP_SPIN_R 9
/* the pop of a step done: the dot's radius 40 % -> 108 % in SLATE_CTL_POP_MS1 (OutCubic), -> 100 % in
   SLATE_CTL_POP_MS2 (Linear) */
#define SLATE_CTL_POP_MS1 260
#define SLATE_CTL_POP_MS2 140
slate_ctl_box_t slate_ctl_step_row(int i, int w);
/* the check of a done step, relative to its row: (18, 24) - (22, 28) - (30, 20) */
void slate_ctl_check(float pts[6]);

#endif
