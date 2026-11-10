# ext-slate

Slate is a retained-mode 2D scene graph and compositor for php-baremetal display panels: you describe a scene of
shapes, text and pixels once, change the properties of its elements, and Slate redraws only what changed, band
by band, at the pace of the panel, with anti-aliased edges, alpha blending and optional gradients, soft shadows
and blur. A screen that does not change costs nothing.

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
| `slate-debug` | `true` / `false` | `false` |

## Documentation

- [Overview](docs/overview.md)
- [Scenes and pages](docs/usage/scenes.md)
- [Elements](docs/usage/elements.md)
- [Effects and build options](docs/usage/effects-and-options.md)
- [Performance](docs/usage/performance.md)
- [Debug](docs/usage/debug.md)
- [C API of the graphics layer](docs/reference/c-api.md)
