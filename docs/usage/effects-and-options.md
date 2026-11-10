---
eyebrow: 'ext-slate · PHP API'
lede:    'Gradients, drop shadows and group blur, each built only when the slate-effects option lists it; slate-debug builds the profiler.'
see_also:
  - { href: './elements.md', meta: 'API' }
  - { href: './performance.md', meta: 'API' }
  - { href: './debug.md', meta: 'API' }
prev: { label: 'Elements', href: './elements.md' }
next: { label: 'Performance', href: './performance.md' }
---

# Effects and build options

## Gradients

<!-- @method name="LinearGradient::__construct(array $stops, float $angle = 0.0)" -->
<!-- @method name="RadialGradient::__construct(array $stops, float $cx = 0.5, float $cy = 0.5, float $r = 0.5)" -->

A gradient fills `$fill` of a `RoundRect` or a `Circle`, or `$color` of an `Arc`. `$stops` holds 2 to 16 stops
`[position, color]` or `[position, color, alpha]`, positions from 0 to 1 (Slate sorts them), colors `0xRRGGBB`
(opaque) or `0xAARRGGBB`. The third value, from 0.0 (transparent) to 1.0 (opaque), replaces the color's alpha:
it is the way to write a fully transparent stop, `[1.0, 0xFFFFFF, 0.0]`. Colors are interpolated
premultiplied, so a fade to transparent keeps its hue, and dithered, so a slow ramp does not band.
Both are immutable (`readonly` properties `$stops`, `$angle`, `$cx`, `$cy`, `$r`) and extend the abstract
`Gradient`; one gradient can fill several elements.

- `LinearGradient`: across the element's box, `$angle` in degrees, 0 left to right, 90 top to bottom.
- `RadialGradient`: centre (`$cx`, `$cy`) and radius `$r` as fractions of the box (an ellipse on a box that is
  not square).

For an `Arc` the box is the one of its whole circle, so the gradient does not move when the arc grows.

<!-- @code-block language="php" label="Gradients" -->
```php
$card = new RoundRect(20, 20, 320, 120, radius: 16,
    fill: new LinearGradient([[0.0, 0x202830], [1.0, 0x405060]], angle: 90));
$ring = new Arc(180, 180, 100, -90, 180, 12,
    new LinearGradient([[0.0, 0x40E0A0], [1.0, 0xE0A040]]));
$glow = new Circle(180, 180, 80,
    fill: new RadialGradient([[0.0, 0x80FFFFFF], [1.0, 0xFFFFFF, 0.0]]));   // white glow fading out
```
<!-- @endcode-block -->

## Shadows

<!-- @method name="Shadow::__construct(int $dx, int $dy, int $blur, int|float $color)" -->

A drop shadow for `$shadow` of a `RoundRect`, a `Circle` or a `Group`: offset (`$dx`, `$dy`) in pixels, `$blur`
in pixels and a color, usually translucent (`0x80000000`). A shape's shadow follows its outline (like CSS
`box-shadow`); a group's shadow follows the content it draws (like CSS `drop-shadow`). An element with a shadow
is drawn through a cache (see [Performance](./performance.md)).

## Blur

`$blur` of a `Group`, in pixels: the group is composed apart, blurred and kept in a cache. Use it for a frosted
panel behind a dialog, or to soften what is behind a popup.

## How blur is measured

`$blur` of `Shadow` and `Group` means what it means in CSS: how far the softness reaches, in pixels. Slate blurs
with three passes of a box blur of radius `r = round(blur / 3)` (at least 1 when `blur` is greater than 0), so
the softness reaches about `3r` pixels past the shape and looks like a gaussian blur of about `r + 0.5` pixels.
The range is 0 to 765.

## Build options

Set in `extra.baremetal.options` of the project:

| Option | Values | Default |
| --- | --- | --- |
| `slate-effects` | a list of `gradients`, `shadows`, `blur` | all three |
| `slate-debug` | `true` or `false` | `false` |

An effect that is not in `slate-effects` is not compiled into the firmware (it saves its code), and the PHP
that would use it throws `\ValueError`:

| Effect left out | What throws | Message |
| --- | --- | --- |
| `gradients` | `new LinearGradient(...)`, `new RadialGradient(...)` | `gradients is not built in: add it to slate-effects in composer.json` |
| `shadows` | `new Shadow(...)` | `shadows is not built in: add it to slate-effects in composer.json` |
| `blur` | `$group->blur = 6` (any value above 0) | `blur is not built in: add it to slate-effects in composer.json` |

`slate-debug` builds the profiler: the frames per second on screen and a line of timings per second on the
console (see [Debug](./debug.md)). Without it the profiler is not compiled.

<!-- @code-block language="json" label="composer.json (excerpt)" -->
```json
{
  "extra": {
    "baremetal": {
      "options": {
        "slate-effects": ["gradients", "shadows"],
        "slate-debug": true
      }
    }
  }
}
```
<!-- @endcode-block -->

An empty list (`"slate-effects": []`) builds none of them.
