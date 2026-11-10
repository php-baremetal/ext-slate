/* raster.h - Slate's anti-aliased shapes and text, drawn into a band (or any slate_buf_t) through slate_gfx().
   Pure C, no ESP-IDF and no PHP.

   Coordinates. Every position is in screen coordinates; the buffer b covers the screen rectangle
   [b->ox, b->ox + b->w) x [b->oy, b->oy + b->h), and nothing outside it or outside clip (screen coordinates,
   half open) is written. A pixel's coverage depends only on its screen position, so a shape drawn band by band
   (bands stacked vertically, each with its oy) gives exactly the same pixels as the same shape drawn into one
   buffer. A gradient row that starts at a different column (a clip edge or a buffer split across x) may differ
   by one RGB565 level, because the gradient steps and dither are accumulated from the row's start.
   Shapes take float coordinates (subpixel positions allowed), on a grid where pixel (i, j) is the square
   [i, i + 1) x [j, j + 1): a rectangle at integer coordinates has crisp edges. Lines are the exception: their
   end points name pixel centres (a line from (x0, y) to (x1, y) with integer y and width 1 fills row y exactly).

   Anti-aliasing. Edge coverage is analytic (a pixel's coverage comes from its distance to the outline, or from
   its exact overlap for straight axis-aligned edges), with no super-sampling. Fully covered opaque spans are
   written with the plain fill; only edge pixels are blended.

   Colours are straight 0xAARRGGBB; a is an extra opacity (0..255) multiplied with the colour's alpha. */
#ifndef SLATE_RASTER_H
#define SLATE_RASTER_H

#include <stdbool.h>
#include <stdint.h>

#include "slate_gfx.h"

struct display_font_entry;   /* display_font.h (ext-display) */

typedef enum { SLATE_CAPS_BUTT = 0, SLATE_CAPS_ROUND = 1 } slate_caps_t;
typedef enum { SLATE_ALIGN_LEFT = 0, SLATE_ALIGN_CENTER = 1, SLATE_ALIGN_RIGHT = 2 } slate_align_t;

/* A gradient over the shape's bounding box (only with the gradients effect built, see slate_config.h: without it a
   gradient paint draws nothing) (for an arc, the bounding box of its whole circle, so the gradient
   does not move when the arc's ends do). Stops as in slate_gfx.h (sorted by at, at most
   SLATE_GRADIENT_STOPS_MAX are used).
   Linear (radial false): angle in degrees, 0 = left to right, 90 = top to bottom (clockwise); t runs from 0 at the
   box corner that comes first in that direction to 1 at the opposite one. cx, cy, r are ignored.
   Radial: centre (x0 + cx * w, y0 + cy * h) of the box, radii r * w and r * h (an ellipse on a non-square box);
   t = 0 at the centre and 1 on that ellipse. angle is ignored. */
typedef struct { const slate_stop_t *stops; int ns; bool radial; float angle, cx, cy, r; } slate_paint_grad_t;
typedef struct { uint32_t argb; const slate_paint_grad_t *grad; } slate_paint_t;   /* grad NULL: solid argb */

/* The graphics table the functions below draw through: NULL (the default) is slate_gfx(); the compositor sets
   another one while it draws into an offscreen image (cache.h) and puts the previous one back. Returns the
   previous table. Not thread-safe: only the compositor task may call the functions of this header (while it has
   the offscreen table set, any slate_buf_t drawn into is taken for the b of a slate_bufa_t); never call them
   from another task, e.g. the PHP one. */
const slate_gfx_ops_t *slate_raster_ops(const slate_gfx_ops_t *ops);

/* Rectangle (x, y, w, h) with corners of the given radius (clamped to half the shorter side; 0 = square corners),
   filled with fill. border > 0 draws a band of that width inside the outline in border_argb (the fill covers what
   is left inside it); border 0: none. w or h <= 0: nothing. With an opaque fill the border is drawn under the
   fill's edge, so the inner edge has no seam; with a translucent fill the two are blended one after the other,
   which leaves the usual faint seam on the inner edge's anti-aliased pixels. */
void slate_draw_roundrect(slate_buf_t *b, slate_rect_t clip, float x, float y, float w, float h, float radius,
                          slate_paint_t fill, float border, uint32_t border_argb, uint8_t a);

/* Disc of centre (cx, cy) and radius r; border as for slate_draw_roundrect. */
void slate_draw_circle(slate_buf_t *b, slate_rect_t clip, float cx, float cy, float r, slate_paint_t fill,
                       float border, uint32_t border_argb, uint8_t a);

/* Stroke of width `width` centred on the circle of centre (cx, cy) and radius r, from from_deg clockwise to to_deg.
   Degrees, 0 = 3 o'clock, 90 = 6 o'clock (screen y grows down). to_deg < from_deg wraps (from 270 to 90 is the
   right half); |to_deg - from_deg| >= 360 is the full ring; to_deg == from_deg draws nothing. caps: butt ends on
   the radius, round ends add half a disc of diameter width. */
void slate_draw_arc(slate_buf_t *b, slate_rect_t clip, float cx, float cy, float r, float from_deg, float to_deg,
                    float width, slate_caps_t caps, slate_paint_t paint, uint8_t a);

/* Segment from (x0, y0) to (x1, y1) (pixel centres, see above), width `width`. Butt caps end at the end points;
   round caps add half a disc of diameter width (a zero-length round line is a dot). */
void slate_draw_line(slate_buf_t *b, slate_rect_t clip, float x0, float y0, float x1, float y1, float width,
                     slate_caps_t caps, uint32_t argb, uint8_t a);

/* One line of UTF-8 text (up to the NUL) in an ext-display font (one entry of display_fonts[]: a GFX font or an
   extended font of 1, 2 or 4 bits per pixel, whose levels are used as coverage). y is the top of the line (the
   baseline is y + the font's ascent). align says what x is: the left end, the centre or the right end of the
   text. max_width > 0: the text is cut (clipped, not wrapped and without an ellipsis) to that many pixels from
   its left end, and alignment uses the cut width; 0: no limit. A codepoint the font lacks is drawn as a box, as
   ext-display does. Invalid UTF-8 counts as U+FFFD. A NULL font or text draws nothing. */
void slate_draw_text(slate_buf_t *b, slate_rect_t clip, int x, int y, const char *utf8,
                     const struct display_font_entry *font, uint32_t argb, slate_align_t align, int max_width,
                     uint8_t a);

/* Screen rectangle that contains every pixel slate_draw_text would write with the same arguments (the line box
   [left, left + width) x [y, y + line height) joined with the ink of every glyph, then cut to max_width). Empty
   (x0 == x1) when nothing would be drawn. */
slate_rect_t slate_text_bbox(int x, int y, const char *utf8, const struct display_font_entry *font,
                             slate_align_t align, int max_width);

#endif
