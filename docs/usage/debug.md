---
eyebrow: 'ext-slate · PHP API'
lede:    'The slate-debug option builds a profiler: frames per second near the top of the screen and one line of timings per second on the console.'
see_also:
  - { href: './performance.md', meta: 'API' }
  - { href: './effects-and-options.md', meta: 'API' }
prev: { label: 'Performance', href: './performance.md' }
next: { label: 'C API of the graphics layer', href: '../reference/c-api.md' }
---

# Debug

Set `"slate-debug": true` in `extra.baremetal.options` and build again. Without the option the profiler is not
compiled and costs nothing.

## On screen

A small counter, centred horizontally an eighth of the height from the top (inside the visible circle of a round
panel), shows the frames per second of the scene. Slate draws it above everything, and redraws only its own box
when the number changes: it does not count as a frame of the scene.

## On the console

Once a second:

<!-- @code-block language="text" label="console" -->
```text
slate: 58.9 fps | frame 9.8 ms (compose 6.1, wait 3.7) | dirty 31% in 3 rects | 42 elements, 17 drawn, 9 culled | cache 2 hit 0 miss | cmds 12
```
<!-- @endcode-block -->

| Field | Meaning |
| --- | --- |
| `fps` | Frames of the scene composed in the last second. |
| `frame` | Average time of a frame, from its start to its last band handed to the panel. |
| `compose` | Of which drawing. |
| `wait` | Of which waiting for the panel (a free band buffer, a band accepted). A high wait means the panel's bus is the limit, not the drawing. |
| `dirty ... in ... rects` | Share of the screen composed per frame, and in how many rectangles. |
| `elements` | Elements alive in the compositor. |
| `drawn`, `culled` | Elements drawn and skipped because an opaque element covers them, per frame (counted once per band they touch). |
| `cache ... hit ... miss` | Cached images reused, and built (or not available), per frame. |
| `cmds` | Changes applied in the second. |

The lines are printed after the frame in progress has handed over its last band, so the console (slow, and
blocking while it writes) never delays a frame or counts in its times.

A second without frames prints its line too, with every value at 0:

<!-- @code-block language="text" label="a still screen" -->
```text
slate: 0.0 fps | frame 0.0 ms (compose 0.0, wait 0.0) | dirty 0% in 0 rects | 42 elements, 0 drawn, 0 culled | cache 0 hit 0 miss | cmds 0
```
<!-- @endcode-block -->

That is the expected state of a screen that does not change: no frame is composed.

## Over budget

A frame longer than the panel's period (16.7 ms at 60 Hz, 1000 divided by the refresh rate) prints, at most
once a second:

<!-- @code-block language="text" label="console" -->
```text
slate: frame 23.4 ms over budget in Home
```
<!-- @endcode-block -->

`Home` is the short class name of the scene shown. Look at what changed in that scene during the second: large
translucent areas, gradients over a big surface, or a cached group whose content changes every frame (see
[Performance](./performance.md)).
