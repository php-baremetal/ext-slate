# Changelog

## 1.1.0

- Added: the PHP API of images. `Image` (`new Image($x, $y, $src, opacity: 1.0)`), an element that draws a `.sli`
  file or a `.png` decoded on the board, loaded in the background: `$src` (an absolute path ending in `.sli` or
  `.png`, in any case; checked at once with `\ValueError` `Baremetal\Slate\Image::$src must be an absolute path`,
  `Baremetal\Slate\Image::$src must end in .sli or .png`, or `png is not built in: add it to slate-images in
  composer.json`), read-only `$w`, `$h`, `$loaded` (updated when the event is delivered, or by `wait()`),
  `wait(int $timeout_ms = 5000): bool` (blocks without delivering other events; false on timeout or failure). Images
  of one path are loaded once and shared; an `Image` of a path in memory is ready at once; the path of a failed image
  is loaded again by the next `Image` or `$src` of it. `Images::preload(array $paths): Preload` keeps the images in
  memory while the `Preload` lives (`$loaded`, `$total`, `$done`, `wait()`). The events `ImageLoaded` (`$path`, `$w`,
  `$h`), `ImageFailed` (`$path`, `$error`, `$message`), `PreloadProgress` (`$loaded`, `$total`) and `PreloadDone`
  (`$failed`) on the bus of ext-events, and the enum `ImageError` (`NotFound`, `Unreadable`, `BadFormat`,
  `Unsupported`, `NoMemory`). The store's listener wakes the compositor when an image is done, so it is drawn
  without waiting for another change; in `init-loop` (no events) the properties still follow at the end of each
  `loop()`. Every Preload gets exactly one PreloadDone, also when a burst of images overflows the event pool (the
  images whose events were dropped are caught up at the return to the event loop, and a refused PreloadDone is
  raised again). Docs: `docs/usage/images.md` (assets, board paths `/app/...` and `/sdcard/...`, memory of on-board PNG),
  the `slate-images` option, performance; IDE stub 1.1.0.
- Added: .sli images, Slate's run-length image format (written by phpbm image:convert): a reader that checks a whole
  file once (header, row table, every command, padding, the opaque flag) and a drawer that composes the image band
  by band with the graphics layer's fill and copy entries, at any opacity and clip (`src/sli.h`: `slate_sli_open`,
  `slate_sli_draw`).
- Added: PNG files decoded on the board into .sli images, byte for byte what phpbm image:convert writes for the
  same file (`src/png.h`: `slate_png_to_sli`): colour types grey, RGB, palette, grey+alpha and RGBA at 8 bits
  (grey and palettes also at 1, 2 and 4), `tRNS`, the five filters, image data in any number of IDAT chunks, every
  CRC and the Adler-32 checked; 16-bit and interlaced images are reported as unsupported. The file is read as a
  stream through a small inflate written for Slate (`src/inflate.h`, no third-party code): the 32 KB window, two
  rows and the result, never the whole image. The .sli encoder is a C API of its own (`src/sli_enc.h`: new, row, finish, abort), and the
  load errors are `slate_img_err_t` in `src/sli.h`.
- Added: the build option `slate-images` (set of `png`, default `["png"]`): without `png` the PNG decoder and the
  inflate code are not built; .sli images are always supported.
- Added: the image store and its loader task (`src/images.h`): images shared by path with counted references
  (`slate_image_get`, `slate_image_ref`, `slate_image_put`, from any task), loaded one at a time by a low-priority
  task of the events HAL that starts with the first request and ends when nothing is left to load (6 KB of stack,
  only while it runs), read with the C standard I/O (.sli as it is, .png decoded) into external RAM, outside the
  cache budget; LOADING, then READY (immutable) or FAILED with `slate_img_err_t`; a listener called once for
  every image done, from the task (or from `slate_image_get` when the task cannot start: NO_MEMORY); an image dropped while it loads is freed at the end, one dropped while it waits is never
  loaded; a failed image leaves the store, so the next request of its path loads it again. The extension (`.png`,
  `.sli`) is matched in any case.
- Added: the IMAGE scene element (`SLATE_EL_IMAGE`, property `SLATE_PROP_IMAGE` with `slate_v_image`): drawn at its
  rounded position with `slate_sli_draw_ops` (the table of the buffer it goes into, so also inside groups with
  opacity, caches and blur), times its opacity; an opaque image at opacity 1 covers what is under it; a still image
  that blends makes the underlay worth keeping. Nothing is drawn or covered until the engine sees the image ready
  (`slate_el_image_sync`): then only its box is composed again. Commands carrying an image hold a reference.

## 1.0.0

- First release: the PHP API `Baremetal\Slate` (`Slate`, `Scene`, the elements `RoundRect`, `Circle`, `Arc`,
  `Line`, `Text`, `Group`, `Canvas`, the enums `Caps` and `Align`, `LinearGradient`, `RadialGradient`,
  `Shadow`), the compositor task on the band path of ext-display 1.1, the software graphics layer with its C API
  (`slate_gfx.h`, overridable per entry), and the build options `slate-effects` (`gradients`, `shadows`, `blur`)
  and `slate-debug`.
- Frames: paced on the panel's TE pulses (or its refresh rate without a TE pin); only the changed parts of the
  screen are composed, in up to 32 dirty regions merged only when that costs fewer pixels; still elements at the
  bottom of the scene are kept as a screen image (the underlay) and copied under the changes; element caches
  (`$cache`, `$shadow`, `$blur`) in PSRAM within a budget; a still screen costs no frame. `set(x: .., y: ..)` is one
  move.
- No tearing on a panel with a TE pin and a known refresh order (ext-display 1.1 `scan`): a frame that changes at
  most half of the screen writes each band where the panel's refresh is not reading, and the whole frame shows in
  one refresh, with the same delay wherever it is on the screen.
- Memory: the two band buffers come from the panel driver (internal RAM); the scratch area for groups with opacity
  and cache builds (internal RAM, at most a band's worth, 43 KB on a 360-wide panel) is taken when a group needs
  it and given back when no frame has needed all of it for a second or another scene is shown, so the rest of the
  program (Wi-Fi) has that RAM meanwhile. The pixel code is compiled for speed (`-O2`, GCC) whatever the project's
  optimization. Frame rates and full-screen times are in the Performance page.
- The panel's `sleep()` pauses the frames (the changes wait) and `wake()` resumes them; after a `wake()` that
  re-initialised the panel (ext-display 1.1 `display_claim_ex()`, `display_invalidate()`) the whole screen is sent
  again.
- A panel that stops answering stops the compositor, and the next call that changes the scene throws
  `\RuntimeException`: `Slate: the panel did not finish a band within 2000 ms`, or `Slate: the panel refused the
  bands (error 0x..)` when the driver marked the panel failed (`BM_ERR_INVALID_STATE`) or refused every band for 4
  frames in a row.
- Properties are validated on every write; references to them, `unset()` and `foreach` by reference over an
  element throw `\Error`. Writes through `array_walk()`, `ArrayObject` or `ArrayIterator` over an element are not
  checked: the compositor refuses such a change when the element is shown.
- With `slate-debug`: an FPS counter on screen and one line of statistics per second on the console, printed
  outside the frames and their times.
- Requires php-baremetal/ext-display ^1.1, php-baremetal/ext-events ^1.2 (the hooks that hand the changes over
  when PHP returns to the event loop), php-baremetal/ext-input ^1.0 and php-baremetal/bm-common ^1.1.
