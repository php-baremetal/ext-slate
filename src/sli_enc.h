/* sli_enc.h - the .sli encoder: RGBA8 rows in, a .sli file (sli.h) out, byte for byte what phpbm writes
   (app/Images/SliEncoder.php). Pure C, no ESP-IDF and no PHP. Always built (the PNG decoder and the tests use it).

   Pixel (x, y), straight alpha a: a = 0 is empty; a = 255 is the colour pm_opaque(r, g, b, th); any other a is
   semi-transparent, colour pm_opaque(div255(r * a), div255(g * a), div255(b * a), th) and alpha a, where th is
   bayer4[y & 3][x & 3] (the dither of gfx_sw.c, anchored at the image origin). A row is cut into maximal spans of
   one kind (empty, opaque, semi-transparent) of at most 32767 pixels; in an opaque span every series of at least 4
   pixels of one RGB565 colour is a "repeated" command and the pixels in between are "different" commands.

   Memory: the row table (4 bytes per row) and the commands, kept in 4 KB pieces from alloc; finish allocates the
   file and copies them into it. */
#ifndef SLATE_SLI_ENC_H
#define SLATE_SLI_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct slate_sli_enc slate_sli_enc_t;

/* An encoder for a w x h image (1..32767 each); its memory and the file's come from alloc (PSRAM on the board) and
   go back with release. NULL: a size out of range, or no memory. */
slate_sli_enc_t *slate_sli_enc_new(int w, int h, void *(*alloc)(size_t), void (*release)(void *));

/* Adds the next row, top to bottom: w * 4 bytes of RGBA8, straight alpha. false: no memory, or every row was given
   already; the encoder then refuses every other row and finish gives NULL. */
bool slate_sli_enc_row(slate_sli_enc_t *e, const uint8_t *rgba);

/* The file (from alloc) and its size, once every row was given; NULL (size 0) otherwise, or with no memory left.
   The encoder is freed in every case. */
uint8_t *slate_sli_enc_finish(slate_sli_enc_t *e, size_t *size);

/* Gives up on the image: the encoder and everything it holds are freed, no file is built (even when every row
   was given). */
void slate_sli_enc_abort(slate_sli_enc_t *e);

#endif
