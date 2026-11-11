/* png.h - PNG to .sli on the board: a PNG read in a stream, row by row, straight into the .sli encoder (sli_enc.h),
   so the result is byte for byte what phpbm image:convert writes for the same file. Pure C, no ESP-IDF and no PHP.
   Built only with SLATE_HAS_PNG (slate-images contains png).

   Read: colour types 0, 2, 3, 4 and 6 at 8 bits, grey and palettes also at 1, 2 and 4 bits; tRNS for grey, RGB and
   palettes (ignored, as libpng does, in grey+alpha and RGBA images); the five filters; image data split into any
   number of IDAT chunks (ancillary chunks between them are skipped); every chunk CRC and the Adler-32 checked.
   Errors: 16-bit samples, Adam7 interlacing, a width or height above 32767 or an unknown critical chunk ->
   SLATE_IMG_UNSUPPORTED; a bad signature, chunk, CRC, header or image data, or a file that ends early ->
   SLATE_IMG_BAD_FORMAT; read() failing -> SLATE_IMG_UNREADABLE; alloc() failing -> SLATE_IMG_NO_MEMORY.
   Memory, all from alloc: the inflate state with its 32 KB window and a 512-byte read buffer (about 37 KB), two
   rows of PNG data and one RGBA8 row (none for 8-bit RGBA), the encoder's output; never the whole image. The
   inflate state and the rows are released before the file is put together, but at that moment the output is held
   twice for a short while (the encoder's 4 KB pieces and the exact-size file they are copied into): the peak is
   about 2x the .sli size (an 800x480 photo: ~770 KB of .sli, ~1.5 MB of PSRAM at the end).
   Stack: the call chain (decode, with a 768-byte PLTE buffer, -> inflate -> output -> encoder -> alloc) needs about
   1.7 KB plus what alloc and read use; the task that calls this must leave room for it. */
#ifndef SLATE_PNG_H
#define SLATE_PNG_H

#include <stddef.h>
#include <stdint.h>

#include "sli.h"

/* Reads a PNG through read(ctx, buf, n) (returns the bytes read, 1..n; 0 at the end; < 0 on an error) up to the
   end of its IEND chunk. SLATE_IMG_OK: *sli is the .sli file (from alloc, to give back with release), *size its
   size. Any error: *sli is NULL, *size 0, and everything allocated was released. */
slate_img_err_t slate_png_to_sli(int (*read)(void *ctx, uint8_t *buf, size_t n), void *ctx,
                                 void *(*alloc)(size_t), void (*release)(void *),
                                 uint8_t **sli, size_t *size);

#endif
