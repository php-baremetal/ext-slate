# ext-slate

Slate is a retained-mode 2D scene graph and compositor for php-baremetal display panels: you describe a scene of
shapes, text and pixels once, change the properties of its elements, and Slate redraws only what changed, band
by band, at the pace of the panel, with anti-aliased edges, alpha blending and optional gradients, soft shadows
and blur, and images loaded in the background from flash or the microSD card. A screen that does not change costs
nothing.

It needs a panel driver with the band path of ext-display 1.1 (on the esp32 SDK, esp32-display-st77916 1.2 or
later), ext-events 1.2 and ext-input 1.0. Platform-neutral: no HAL of its own.

## Install

```bash
composer require php-baremetal/ext-slate
```

## Example

```php
<?php
use Baremetal\Slate\{Slate, Scene, RoundRect, Arc, Text, Align};

final class Home extends Scene
{
    public Text $temp;

    protected function build(): void
    {
        $this->add(
            new RoundRect(20, 20, 320, 120, radius: 16, fill: 0x202830),
            $this->temp = new Text(180, 60, '92 °C', align: Align::Center),
            new Arc(180, 260, 60, from: -90, to: 180, width: 8, color: 0xE0A040),
        );
    }
}

$slate = Slate::onboard();   // the board's display, and its touch screen when it has one
$slate->show($home = new Home());
$home->temp->text = '93 °C';   // drawn at the next frame
```

## Build options

| Option | Values | Default |
| --- | --- | --- |
| `slate-effects` | set of `gradients`, `shadows`, `blur` | all three |
| `slate-images` | set of `png` (formats decoded on the board; `.sli` always) | `png` |
| `slate-debug` | `true` / `false` | `false` |

## Documentation

- [Overview](docs/overview.md)
- [Scenes and pages](docs/usage/scenes.md)
- [Elements](docs/usage/elements.md)
- [Images](docs/usage/images.md)
- [Animations](docs/usage/animations.md)
- [Scroll](docs/usage/scroll.md)
- [Effects and build options](docs/usage/effects-and-options.md)
- [Performance](docs/usage/performance.md)
- [Debug](docs/usage/debug.md)
- [C API of the graphics layer](docs/reference/c-api.md)

## AI usage

Unlike the other packages of the php-baremetal ecosystem, where AI is a tool on top of human engineering, this one
was built largely by AI. It is not laziness. A compositor like Slate is a huge amount of work for a single
maintainer: anti-aliased rasterisation, alpha blending and gradients with exact rounding, blur and shadows,
dirty-area tracking, caching, a render task racing the panel's scan line, all tuned to a microcontroller's few
hundred kilobytes of fast memory. It also needs specialised knowledge of computer graphics and real-time
rendering that cannot be learned in a reasonable time alongside everything else the project needs.

Could one person do it all by hand? Sure: give them ten years, no day job and a lifetime supply of coffee.
`¯\_(ツ)_/¯`

So an AI assistant wrote most of the code, tests and documentation, and did much of the design and performance
work under the maintainer's direction. The maintainer set the requirements, made the design and trade-off
decisions, and checked the results: host tests that compare every frame pixel by pixel with a full redraw, and
runs on real hardware. This exception applies to this package only: the details are in
**[AI_USAGE.md](AI_USAGE.md)**.

Not for you? No worries at all — nobody's forcing it on you, and a free, open-source project you
didn't spend a minute or a cent on owes you no explanation anyway. Take it or leave it, no hard feelings.

— Written with Claude (Opus 5.5, by Anthropic)
