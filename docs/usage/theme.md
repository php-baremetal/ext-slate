---
eyebrow: 'ext-slate · PHP API'
lede:    'A Theme holds the colours, the corner radius, the heights of the three sizes and the fonts of the controls. Set it with Slate::theme() before the first show(); a control reads it when it is made.'
see_also:
  - { href: './controls.md', meta: 'API' }
  - { href: './elements.md', meta: 'API' }
prev: { label: 'Controls', href: './controls.md' }
next: { label: 'Effects and build options', href: './effects-and-options.md' }
---

# Theme

The [controls](./controls.md) draw themselves with the tokens of a `Theme`: colours, the radius of
`Shape::Rounded`, the heights of `Size::Small`, `Size::Medium` and `Size::Large`, and five fonts. A Slate starts with
the defaults, a neutral dark theme; `Slate::theme()` replaces it before the first `show()`.

<!-- @code-block language="php" label="A theme of your own" -->
```php
<?php
use Baremetal\Slate\{Slate, Theme, Scene, RoundRect, Button, Variant, Size, Shape};

$slate = Slate::onboard();
$slate->theme(new Theme(
    bg: 0x06080C, surface: 0x0E131A, raised: 0x141B24, well: 0x0A0E13,
    border: 0x1E2732, borderStrong: 0x2A3542,
    text: 0xE8EEF5, textMuted: 0xAEB9C7, textDim: 0x4A5666,
    accent: 0xFFB347, onAccent: 0x06080C, danger: 0xFF6B5E, info: 0x5CE1FF, success: 0x4ADE80,
    radius: 14, medium: 52,
    font: 'dejavu-sans-16', fontSmall: 'dejavu-sans-12', fontTitle: 'dejavu-sans-24',
    fontMono: 'dejavu-mono-16', fontDisplay: 'dejavu-mono-40',
));   // before the first show(); the tokens left out keep the defaults

$t = $slate->theme;   // to draw the rest with the same colours
$scene = new Scene();
$scene->add(
    new RoundRect(0, 0, 360, 360, fill: $t->bg),
    new Button(40, 200, 'Start', Variant::Primary, Size::Large, w: 280, shape: Shape::Pill),
);
$slate->show($scene);
```
<!-- @endcode-block -->

The `dejavu-sans-*` fonts come from the `php-baremetal/font-dejavu` package; `dejavu-mono-16` and `dejavu-mono-40`
are baked by the project from `DejaVuSansMono.ttf` (`extra.baremetal.fonts.bake` of its `composer.json`). See
[Fonts](#fonts). The `06-display/slate-controls` example uses this theme, with its fonts and icons.

## Theme

<!-- @method name="Theme::__construct(int $bg = 0x0F1115, int $surface = 0x161A20, int $raised = 0x1C2129, int $well = 0x111419, int $border = 0x262C35, int $borderStrong = 0x343B46, int $text = 0xE8EAED, int $textMuted = 0xA0A7B2, int $textDim = 0x5C6370, int $accent = 0x4C8DFF, int $onAccent = 0xFFFFFF, int $danger = 0xFF5D5D, int $info = 0x5CC8FF, int $success = 0x4ADE80, int $radius = 12, int $small = 40, int $medium = 48, int $large = 56, string $font = 'default', string $fontSmall = 'default', string $fontTitle = 'default', string $fontMono = 'default', string $fontDisplay = 'default')" -->

`final class Theme`. Pass the tokens by name: the ones left out keep their default. Every token is a
`public readonly` property of the same name, so `$theme->accent` reads it.

| Token | Default | Use |
| --- | --- | --- |
| `bg` | `0x0F1115` | The scene's background. Slate does not paint the scene: use it for a full-screen `RoundRect`. |
| `surface` | `0x161A20` | Panels: a dialog, a toast, raised rows. |
| `raised` | `0x1C2129` | A `Secondary` button, a chip not chosen. |
| `well` | `0x111419` | Recesses: the track of a segmented control, the empty part of a slider. |
| `border` | `0x262C35` | Borders and dividers. |
| `borderStrong` | `0x343B46` | The border of a `Secondary` button. |
| `text` | `0xE8EAED` | The main text. |
| `textMuted` | `0xA0A7B2` | Secondary text, `Ghost` buttons, subtitles. |
| `textDim` | `0x5C6370` | Dimmed text. |
| `accent` | `0x4C8DFF` | The accent: `Primary` and `Outline` buttons. |
| `onAccent` | `0xFFFFFF` | Text on the accent and on `danger`. |
| `danger` | `0xFF5D5D` | A `Danger` button. |
| `info` | `0x5CC8FF` | An informative colour, for the application's own use. |
| `success` | `0x4ADE80` | Done steps. |
| `radius` | `12` | The radius of `Shape::Rounded`, 0 to 64. |
| `small` / `medium` / `large` | `40` / `48` / `56` | The heights of the sizes, 24 to 128. |
| `font` | `'default'` | The text of the controls. |
| `fontSmall` | `'default'` | Subtitles and small labels. |
| `fontTitle` | `'default'` | Titles (a dialog's). |
| `fontMono` | `'default'` | Values (a setting row's, a slider's). |
| `fontDisplay` | `'default'` | A large number (the centre of a progress ring). |

Colours are `0xRRGGBB`, opaque. `Size`, `Variant`, `Shape` and `ChoiceStyle` are the enums of the controls: see
[Controls](./controls.md).

## Setting the theme

<!-- @method name="Slate::theme(Theme $theme): static" returns="static" -->

Sets the theme before the first `show()` and returns the Slate. Without a call the theme is `new Theme()`.
`Slate::$theme` (`public readonly Theme`) reads it: draw the rest of the screen with the same colours.

A control reads the theme **when it is made**, so set the theme first, right after `new Slate()`: a control made
before `theme()` keeps the theme of that moment. Changing the theme of a running screen is not supported: after the
first `show()`, `theme()` throws.

## Fonts

A font token is the name of a font of the firmware, as `Text::$font`: the fonts listed in
`extra.baremetal.fonts` of the project. `'default'` is ext-display's own font. The `php-baremetal/font-dejavu`
package adds `'dejavu-sans-12'`, `'dejavu-sans-16'` and `'dejavu-sans-24'`; the fonts the project bakes from its
own TTF files (`extra.baremetal.fonts.bake`) have the names it gives them.

## Errors

| When | Throws | Message |
| --- | --- | --- |
| A colour outside `0..0xFFFFFF` | `\ValueError` | `Baremetal\Slate\Theme::$<token> must be a color, 0xRRGGBB` |
| `$radius` outside 0..64 | `\ValueError` | `Baremetal\Slate\Theme::$radius must be between 0 and 64` |
| A height outside 24..128 | `\ValueError` | `Baremetal\Slate\Theme::$small must be between 24 and 128` (and `$medium`, `$large`) |
| A font the firmware does not have | `\ValueError` | `Baremetal\Slate\Theme::$font must be a font of the firmware: unknown font '<name>' (available: <names>)` (and the other fonts) |
| `Slate::theme()` after the first `show()` | `\Error` | `the theme is set before the first show()` |
| A write of a token | `\Error` | PHP's error for a readonly property |

The constructor checks the tokens in the order of its parameters and throws for the first one out of range.
