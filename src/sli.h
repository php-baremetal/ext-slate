/* sli.h - .sli images: Slate's run-length image format, read and drawn. Pure C, no ESP-IDF and no PHP.

   The file (all little-endian), as phpbm and the board's PNG decoder write it:
   - a 16-byte header: "SLI1", version 1 (u8), flags (u8: bit 0 has_alpha, bit 1 opaque, exactly one of the two),
     width and height (u16, 1..32767), 0 (u16), the size of the data in bytes (u32);
   - the row table: one u32 per row, the row's offset from the start of the data (16 + 4 * height); each row starts
     at a multiple of 4, with zero bytes between rows and none after the last;
   - the rows: commands that cover the width exactly. Command byte: bits 7..6 the kind (0 empty, 1 one repeated
     opaque colour, 2 different opaque colours, 3 semi-transparent), bits 5..0 the count 1..63, or 0 followed by a
     u16 count 64..32767. Then: empty, nothing; repeated, the colour (u16, at any offset); different, a zero byte
     when needed to put the colours at an even offset from the start of the data, then the colours; semi-transparent,
     the same padding, the premultiplied colours, then one alpha byte per pixel.
   Colours are RGB565 native (red in bits 15..11), as slate_buf_t. */
#ifndef SLATE_SLI_H
#define SLATE_SLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "slate_gfx.h"

/* Why an image could not be loaded (the PHP enum ImageError has the same cases). */
typedef enum {
    SLATE_IMG_OK = 0,
    SLATE_IMG_NOT_FOUND,     /* no such file */
    SLATE_IMG_UNREADABLE,    /* the file could not be read */
    SLATE_IMG_BAD_FORMAT,    /* not a valid image of its format (signature, chunk, CRC, data) */
    SLATE_IMG_UNSUPPORTED,   /* valid, but not decoded on the board (16 bits, interlaced, larger than 32767) */
    SLATE_IMG_NO_MEMORY,     /* an allocation failed */
} slate_img_err_t;

/* A .sli image in memory (the whole file, as read). data is not copied: it must stay while the image is used. */
typedef struct { const uint8_t *data; size_t size; uint16_t w, h; bool opaque; } slate_sli_t;

/* Checks the whole file once (header, row table, every command within its row and within size, padding, the
   opaque flag against the pixels) and fills im. false: not a valid .sli, im untouched. data must be at an even
   address (colours are read 16 bits at a time); an odd one is refused. */
bool slate_sli_open(slate_sli_t *im, const uint8_t *data, size_t size);

/* Draws the image with its top-left pixel at (x, y) of b (buffer coordinates), limited to clip and to the buffer,
   times opacity a (0..255), with slate_gfx() entries: empty -> skipped; repeated -> fill (fill_a when a < 255);
   different -> copy (copy_a with a uniform alpha when a < 255); semi-transparent -> copy_a (premultiplied, the
   command's alpha row, times a). The result equals copy_a of each pixel on its own. im: opened by slate_sli_open
   (draw does no checks of its own). */
void slate_sli_draw(slate_buf_t *b, const slate_sli_t *im, int x, int y, uint8_t a, slate_rect_t clip);
/* The same through the entries of g (the compositor passes the table of the buffer it draws into: an offscreen
   RGB565 + A8 strip has its own, cache.h). */
void slate_sli_draw_ops(const slate_gfx_ops_t *g, slate_buf_t *b, const slate_sli_t *im, int x, int y, uint8_t a,
                        slate_rect_t clip);

#endif
