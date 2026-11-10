/* slate_gfx.h - Slate's graphics layer: fills, alpha blending, copies, gradient rows and box blur on RGB565.
   Pure C, no ESP-IDF and no PHP. The software table is always present; a hardware layer replaces single
   entries with slate_gfx_override().

   Pixel format. slate_buf_t and every pixel array of this header hold RGB565 in the CPU's NATIVE order
   (a uint16_t with red in bits 15..11, green in 10..5, blue in 4..0). The panel wants the high byte first
   (see display_panel.h), so the byte swap happens once, when a finished band is handed to the panel
   (slate_swap565 / slate_swap565_row), not in every blend. Unlike the display framebuffer, which is pushed
   untouched and so stores swapped pixels, a band is read back by every blend, and swapping on each read would
   cost more than one pass at hand-off. Cached images (RGB565 + A8) are native too.

   Alpha. Alpha is 8 bit (0 transparent, 255 opaque). Colours of RGB565 + A8 images (copy_a, blur) are
   premultiplied: colour = straight colour * alpha / 255.

   Blend formula (all integer). With div255(x) = (x + 128 + ((x + 128) >> 8)) >> 8, which is x / 255 rounded
   to nearest for 0 <= x <= 65535, every channel c of the destination d and the source s, taken on its own
   range (red 0..31, green 0..63, blue 0..31), with alpha a:
       straight:      c = div255(s * a + d * (255 - a))
       premultiplied: c = min(div255(s * a' + d * (255 - A)), max), where a' is the extra opacity, and
                      A = div255(alpha * a') is the pixel's resulting alpha (a' = 255: s + div255(d * (255 - A)))
   a = 0 leaves d unchanged and a = 255 gives s exactly. A coverage mask multiplies the alpha:
   a_eff = div255(cov * a).

   Clipping. fill, fill_a and fill_mask clip to the buffer only (the caller intersects r with its own clip;
   for fill_mask cov keeps following the rect as given). copy and copy_a take a clip rect and also clip to the
   buffer. gradient_row has no clip beyond the buffer: the caller pre-clips x and n, cov[i] follows pixel i.
   Pixel arrays never overlap the destination except in copy, which is memmove-safe. */
#ifndef SLATE_GFX_H
#define SLATE_GFX_H

#include <stddef.h>
#include <stdint.h>

/* RGB565 native order, stride in pixels. ox, oy: screen coordinates of px[0] (0, 0 for a buffer that is the whole
   screen and for images that are not on screen). Only the gradient dither uses them: its pattern is anchored to
   ((x + ox) & 3, (y + oy) & 3), so a gradient drawn band by band, or redrawn in part, matches its neighbours.
   Whoever creates a band or a cache buffer sets them. */
typedef struct { uint16_t *px; int w, h, stride; int ox, oy; } slate_buf_t;
typedef struct { int x0, y0, x1, y1; } slate_rect_t;              /* half open: [x0,x1) x [y0,y1) */
typedef struct { uint32_t argb; float at; } slate_stop_t;         /* straight (not premultiplied) 0xAARRGGBB */

#define SLATE_GRADIENT_STOPS_MAX 16   /* more stops than this are ignored */
#define SLATE_BLUR_RADIUS_MAX 255     /* larger radii are clamped */
#define SLATE_BLUR_TMP(w, h) ((size_t)(w) * (size_t)(h) * 3)     /* tmp bytes for blur of RGB565 + A8 */
#define SLATE_BLUR_TMP_A(w, h) ((size_t)(w) * (size_t)(h))       /* tmp bytes for an alpha-only blur */

typedef struct {
    /* Opaque fill of r (clipped to the buffer). */
    void (*fill)(slate_buf_t *b, slate_rect_t r, uint16_t rgb565);
    /* Fill with alpha a. */
    void (*fill_a)(slate_buf_t *b, slate_rect_t r, uint16_t rgb565, uint8_t a);
    /* Coverage mask: dst = blend(dst, color, cov[i] * a / 255); cov has r's size (r as given, not clipped),
       stride cov_stride, row 0 of cov is row r.y0. */
    void (*fill_mask)(slate_buf_t *b, slate_rect_t r, uint16_t rgb565, uint8_t a, const uint8_t *cov, int cov_stride);
    /* Opaque copy of the sw x sh image src to (x, y), limited to clip (and to the buffer). */
    void (*copy)(slate_buf_t *b, int x, int y, const uint16_t *src, int sw, int sh, int sstride, slate_rect_t clip);
    /* src RGB565 + alpha plane A8 (premultiplied colours), times a. The alpha plane has the same stride as src
       (sstride, in elements) and the same size. */
    void (*copy_a)(slate_buf_t *b, int x, int y, const uint16_t *src, const uint8_t *alpha, int sw, int sh,
                   int sstride, uint8_t a, slate_rect_t clip);
    /* One row of a gradient: n pixels at (x, y). t is anchored to the screen, not to x: t0 is t at screen
       column 0 of this row and dt the step per pixel, so the pixel at buffer column x + i (screen column
       X = x + i + ox) has t = t0 + X * dt. Every pixel is a function of (X, y + oy, t0, dt) only: a row drawn
       whole, in pieces, clipped or split across buffers gives identical pixels for the same t0 and dt.
       Stops sorted by at, colours straight ARGB; t before the first stop or after the last takes that stop's
       colour. Colours are interpolated premultiplied (a fade to 0x00000000 stays the same hue, it does not go
       grey) and reduced to RGB565 with a 4x4 ordered dither keyed on screen position (x + ox, y + oy), always on,
       so a smooth ramp does not band; the result is therefore within +-1 level of the plain truncated value.
       cov (n bytes, cov[i] for pixel i) may be NULL. The per-pixel loop is integer only: inside each span
       between two stops every channel is an 8.16 affine form of X, stepped by one add per pixel.
       t0 and the stops' at are clamped to +-4096 and dt to +-128 per pixel (internal fixed point), so a degenerate
       gradient (narrower than 1/128 px) is not exact.
       NULL when the gradients effect is not built (slate_config.h). */
    void (*gradient_row)(slate_buf_t *b, int x, int y, int n, float t0, float dt, const slate_stop_t *stops, int ns,
                         const uint8_t *cov);
    /* Box blur, three passes (horizontal and vertical each), radius r, on an RGB565 + A8 image in place; the image
       is tightly packed (stride == w). Pixels outside the image count as transparent. tmp: SLATE_BLUR_TMP(w, h)
       = w*h*3 bytes, 2-byte aligned. Colours are blurred on their 5/6/5 range and alpha on 8 bits, so very
       faint tails round to zero. r <= 0: untouched.
       px == NULL: alpha only (a shadow or glow is a uniform colour drawn through fill_mask with the blurred
       alpha as coverage): only the A8 plane is blurred, tmp is SLATE_BLUR_TMP_A(w, h) = w*h bytes, and the
       result equals the alpha plane of the full blur. Hardware overrides must keep this contract.
       NULL when neither the blur nor the shadows effect is built (slate_config.h). */
    void (*blur)(uint16_t *px, uint8_t *alpha, int w, int h, int r, void *tmp);
} slate_gfx_ops_t;

const slate_gfx_ops_t *slate_gfx(void);                   /* the active table */
/* Non-NULL entries replace the software ones; NULL restores software. Each call starts again from the software
   table. Not thread-safe: call it before the compositor task starts. */
void slate_gfx_override(const slate_gfx_ops_t *hw);
uint16_t slate_rgb565(uint32_t rgb);                      /* 0xRRGGBB -> RGB565 (top bits of each channel) */

/* Byte swap for the panel (high byte first on the wire); dst == src (in place) is allowed. */
static inline uint16_t slate_swap565(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
void slate_swap565_row(uint16_t *dst, const uint16_t *src, int n);

#endif
