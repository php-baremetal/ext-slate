---
eyebrow: 'ext-slate · Reference'
lede:    'slate_gfx.h: the graphics layer every pixel of Slate goes through (fills, blending, copies, gradient rows, box blur on RGB565); a hardware layer replaces single entries.'
see_also:
  - { href: '../usage/performance.md', meta: 'API' }
  - { href: 'package:php-baremetal/ext-display#docs/reference/c-api.md', meta: 'Extension', label: 'ext-display C API' }
prev: { label: 'Debug', href: '../usage/debug.md' }
next: { label: 'No next page', href: '#' }
---

# C API of the graphics layer

Slate draws through a table of functions, `slate_gfx_ops_t`, declared in `include/slate_gfx.h`. The software
table (plain C, portable) is always built. A platform package can replace single entries with a faster version
(SIMD instructions, a 2D engine with DMA) by calling `slate_gfx_override()`; the entries it leaves NULL stay in
software. The rasterizers and the compositor never write a pixel any other way, so an override speeds up every
element that uses that entry.

The panel side (bands, buffers, TE) is not here: Slate drives the panel through the band path of the panel
contract of ext-display (`display_band.h`, see the C API of ext-display).

## Pixel format

| Rule | Meaning |
| --- | --- |
| RGB565, native order | A `uint16_t` with red in bits 15..11, green 10..5, blue 4..0, in the CPU's byte order. The swap to the panel's order (high byte first) happens once per band, when the band is handed to the panel (`slate_swap565_row()`). |
| `slate_buf_t` | `{ uint16_t *px; int w, h, stride; int ox, oy; }`: `stride` in pixels; `ox`, `oy` the screen position of `px[0]`, which anchors the gradient dither, so a gradient drawn band by band matches across bands. |
| `slate_rect_t` | `{ int x0, y0, x1, y1; }`, half open: `[x0, x1) x [y0, y1)`. |
| Alpha | 8 bits, 0 transparent, 255 opaque. Images with an alpha plane (A8 next to the RGB565, same stride) have premultiplied colors. |
| Blend | Per channel on its own range: `c = div255(s * a + d * (255 - a))` with `div255(x) = (x + 128 + ((x + 128) >> 8)) >> 8`; `a = 0` leaves the destination, `a = 255` writes the source exactly. A coverage mask multiplies the alpha: `a_eff = div255(cov * a)`. |

## The table

| Entry | Contract |
| --- | --- |
| `void (*fill)(slate_buf_t *b, slate_rect_t r, uint16_t rgb565)` | Opaque fill of `r`, clipped to the buffer. |
| `void (*fill_a)(slate_buf_t *b, slate_rect_t r, uint16_t rgb565, uint8_t a)` | Fill blended with alpha `a`. |
| `void (*fill_mask)(slate_buf_t *b, slate_rect_t r, uint16_t rgb565, uint8_t a, const uint8_t *cov, int cov_stride)` | Fill through a coverage mask: `cov` has the size of `r` as given (not clipped), row 0 is row `r.y0`. Anti-aliased edges, text and shadows go through it. |
| `void (*copy)(slate_buf_t *b, int x, int y, const uint16_t *src, int sw, int sh, int sstride, slate_rect_t clip)` | Opaque copy of an image to (`x`, `y`), limited to `clip` and the buffer; source and destination may overlap. |
| `void (*copy_a)(slate_buf_t *b, int x, int y, const uint16_t *src, const uint8_t *alpha, int sw, int sh, int sstride, uint8_t a, slate_rect_t clip)` | Copy of a premultiplied RGB565 + A8 image times `a`: cached groups and groups with opacity. |
| `void (*gradient_row)(slate_buf_t *b, int x, int y, int n, float t0, float dt, const slate_stop_t *stops, int ns, const uint8_t *cov)` | One row of a gradient: pixel `x + i` (screen column `X = x + i + ox`) takes `t = t0 + X * dt`; stops sorted, straight colors, interpolated premultiplied, reduced to RGB565 with a 4x4 ordered dither keyed on the screen position. Every pixel must depend only on (`X`, `y + oy`, `t0`, `dt`), so a row drawn in pieces gives the same pixels. `cov` may be NULL. NULL when the gradients effect is not built. |
| `void (*blur)(uint16_t *px, uint8_t *alpha, int w, int h, int r, void *tmp)` | Three-pass box blur of radius `r` of a packed RGB565 + A8 image, in place; outside the image counts as transparent. `tmp`: `SLATE_BLUR_TMP(w, h)` bytes. `px` NULL: only the alpha plane (shadows), `tmp` `SLATE_BLUR_TMP_A(w, h)` bytes, with the same result as the alpha plane of the full blur. NULL when neither blur nor shadows are built. |

Limits: `SLATE_GRADIENT_STOPS_MAX` (16) stops are used, `SLATE_BLUR_RADIUS_MAX` (255) is the largest radius.

## Functions

| Function | Use |
| --- | --- |
| `const slate_gfx_ops_t *slate_gfx(void)` | The active table. |
| `void slate_gfx_override(const slate_gfx_ops_t *hw)` | Non-NULL entries of `hw` replace the software ones; NULL restores the software table. Each call starts again from software. Not thread-safe: call it before the compositor starts, for example from the MINIT of the platform package, never while a Slate runs. |
| `uint16_t slate_rgb565(uint32_t rgb)` | `0xRRGGBB` to native RGB565 (the top bits of each channel). |
| `uint16_t slate_swap565(uint16_t c)`, `void slate_swap565_row(uint16_t *dst, const uint16_t *src, int n)` | Byte swap for the panel; `dst == src` allowed. |

## Writing an override

<!-- @code-block language="c" label="a faster fill" -->
```c
#include "slate_gfx.h"

static void my_fill(slate_buf_t *b, slate_rect_t r, uint16_t c)
{
    /* clip r to the buffer, then write rows of c (a DMA 2D fill, SIMD stores, ...) */
}

static const slate_gfx_ops_t my_ops = { .fill = my_fill };   /* everything else stays software */

PHP_MINIT_FUNCTION(my_gfx)
{
    slate_gfx_override(&my_ops);
    return SUCCESS;
}
```
<!-- @endcode-block -->

An override must give exactly the pixels the contract above describes (the software table is the reference),
work on any `slate_buf_t` (bands in internal RAM, caches in external RAM) and be callable from the compositor's
task. Slate's bands are in internal RAM: an entry that blends never reads external RAM there.
