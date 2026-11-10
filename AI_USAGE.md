# AI Usage Policy — ext-slate

This document applies to **this package only**. The rest of php-baremetal follows the project-wide policy (see
`AI_USAGE.md` in [php-esp32-sdk](https://github.com/php-baremetal/php-esp32-sdk)), where AI is a tool on top of human
engineering. ext-slate is the exception that policy foresees: a package that is genuinely AI-heavy, so it says so
openly.

## Why

Not laziness. A compositor like Slate is a huge amount of work for a single maintainer: anti-aliased rasterisation,
alpha blending and gradients with exact rounding, blur and shadows, dirty-area tracking, caching, a render task
racing the panel's scan line, all tuned to a microcontroller's few hundred kilobytes of fast memory. It also needs
specialised knowledge of computer graphics and real-time rendering that cannot be learned in a reasonable time
alongside everything else the project needs. With AI doing the bulk of the work under close direction and
verification, the ecosystem gets a real UI layer instead of none.

## How AI was used here

- **AI did most of the work.** An AI assistant (Claude, by Anthropic) wrote most of the C code (scene, compositor,
  rasteriser, effects, engine task, PHP classes), the host tests, the benches and the documentation. It also did
  much of the design work and the performance rounds: profiling on the board, the dirty-area strategy, the
  underlay, tear-free band scheduling, the memory layout.
- **The maintainer directed it.** The requirements, the API shape, the design and trade-off decisions (what to
  keep, what to drop, how much memory Slate may take, when performance is good enough) are the maintainer's. Every
  design and plan was approved before it was implemented, and every change that touched documented behaviour, a
  default or a memory budget was the maintainer's call.
- **Verification is not optional.** The pixels are checked, not trusted: reference images that must stay
  byte-identical, host tests that compare thousands of incrementally composed frames with a full redraw pixel by
  pixel, deliberate mutations to prove those tests catch bugs, and runs on real hardware (Waveshare
  ESP32-S3-Touch-LCD-1.85B) for frame rate, tearing, memory and Wi-Fi coexistence. The maintainer watched the
  board for what only eyes can judge.

## What this means for you

The code works and is tested, but it was not written line by line by a human. Read it with that in mind, report
what looks wrong, and expect the maintainer to review and verify changes rather than to have authored them.

## Policy for contributions

The project-wide rules apply unchanged: understand and stand behind what you submit, verify behaviour changes on a
real board, keep changes small and reviewable, respect licences and provenance. Contributions are under this
package's [MIT License](LICENSE). For this package in particular, a change to the compositor must keep the
reference images byte-identical and the incremental-vs-full-redraw tests green.

If a contribution is substantially AI-generated, a one-line note in the PR is appreciated, so review can weigh it
accordingly. Honesty here is valued, never penalised.
