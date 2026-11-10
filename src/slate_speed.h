/* slate_speed.h - included first by Slate's pixel code (gfx_sw.c, raster.c, cache.c, compose.c) and by the code that
   applies every change (scene.c, dirty.c): that code is compiled optimized for speed (-O2) whatever optimization the
   project builds with. Firmware projects are often built for size (-Os), which makes the per-pixel loops several
   times slower; the rest of the package (the PHP API, the engine) keeps the project's setting. GCC only: other compilers keep the project's setting.
   Its functions also start on 64-byte boundaries: some targets are built without the alignment of loop bodies
   (Xtensa: -mno-target-align), and then the speed of a pixel loop depends on where the linker happens to put its
   function, so code added anywhere else could slow it down (measured: up to 30 % on a full-screen fill). Aligned,
   the loops sit the same way in every build. */
#ifndef SLATE_SPEED_H
#define SLATE_SPEED_H

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2", "align-functions=64")
#endif

#endif
