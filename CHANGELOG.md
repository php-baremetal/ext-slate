# Changelog

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
