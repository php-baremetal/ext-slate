---
eyebrow: 'ext-slate · PHP API'
lede:    'Ready-made controls drawn with the theme: one PHP object each, its parts in the compositor. Control is their base, and the base of a control of your own: $enabled, the pressed look, onChange() and changed(). Rows of settings, dialogs, toasts, progress and steps.'
see_also:
  - { href: './theme.md', meta: 'API' }
  - { href: './touch.md', meta: 'API' }
  - { href: './elements.md', meta: 'API' }
prev: { label: 'Touch', href: './touch.md' }
next: { label: 'Theme', href: './theme.md' }
---

# Controls

A control is an element that the user works with or looks at: a button, a toggle, a slider, a choice, a row of
settings, a dialog, a progress ring or bar, a spinner, a list of steps. For PHP it is
**one object**: its parts (a background, a text, an icon) live in the compositor as elements without a PHP object
each, so a screen of controls costs little memory and the compositor redraws only the part that changed. A control
draws itself with the [theme](./theme.md), takes the touch as a whole (its area is the union of its parts) and goes
in a scene, a group or a `Scroll` like any element. The `06-display/slate-controls` example shows them all on a
360 x 360 round panel; what they cost is in [Performance](./performance.md#controls).

<!-- @code-block language="php" label="Buttons" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Button, Variant, Size, Shape, Touch, TouchType};

$slate = Slate::onboard();   // the Slate (and its theme) first: a control needs it when it is made
$start = new Button(40, 200, 'Start', Variant::Primary, Size::Large, w: 280, shape: Shape::Pill);
$start->on(TouchType::Tap, fn (Touch $t) => brew());
$cancel = new Button(40, 270, 'Cancel', Variant::Ghost, w: 280);
$slate->show((new Scene())->add($start, $cancel));
```
<!-- @endcode-block -->

## Control

`abstract class Control extends Group`. What every control has:

| Member | Meaning |
| --- | --- |
| `public bool $touchable = true` | A control takes touches by default. |
| `public bool $enabled = true` | `false`: the control is drawn at 0.4 times its `$opacity` (which keeps the value written) and takes no new touch; a touch under way gets `Cancel`. Writing `$enabled` sends the control's opacity again, which stops a tween of `$opacity` running on it (an `animate()` or `repeat` of the opacity ends where it is). |
| `protected readonly Theme $theme` | The running Slate's theme when the control was made. |
| `public function onChange(mixed $value): void {}` | The value changed by the user's gesture. Empty: override it. |
| `public function changed(?\Closure $fn): static` | The callback of the user's changes (one; a new one replaces it, `null` removes it). It runs after `onChange()`. |
| `protected function notify(mixed $value): void` | Calls `onChange($value)`, then the callback of `changed()`. |
| `protected function pressed(bool $down): void {}` | `true` at `Press`, `false` at `Release` and `Cancel`, also when `$enabled` turns `false` during the touch. |

<!-- @method name="Control::__construct(int|float $x = 0, int|float $y = 0)" -->

A control needs a running Slate when it is made (`new Slate()` first), and reads the Slate's theme then.

- **The pressed look.** Slate's controls show it from `Press` to `Release` or `Cancel`: their fill goes 12 %
  toward white in 80 ms, and back (a fill that is not opaque gets the `text` colour at 6 % laid over it). Nothing
  scales.
- **Notifications are for the user's gestures only.** A change made by the code (a write of a property) is drawn,
  but `onChange()` and the callback of `changed()` do not run. Of a gesture, the control's behaviour comes first,
  then its method (`onTap()`...), then its callback of `on()`; an exception stops there, as in any touch handler
  (see [Touch](./touch.md)).
- **`add()` and `remove()`** belong to the controls of your own (classes that extend `Control` directly): Slate's
  controls build their own parts and throw `\Error`.
- **Properties of Slate's controls in your subclass.** A subclass of `Button` (and the others) that declares a property
  of the control again (`public string $text = 'x';`) writes it as a plain PHP property: the control is not laid out
  or redrawn again. Use the inherited properties, and add your own under other names.
- **One constructor call.** Calling `parent::__construct()` (or a control's constructor) twice on the same object throws
  `\Error`: `<Class> is already constructed`.
- **`$w` and `$h`** of Slate's controls read the size laid out (the width given or computed, the height from the
  theme) and cannot be written. A control of your own keeps those of `Group`.
- The touch methods and callbacks of [Touch](./touch.md) (`onTap()`, `on(TouchType::Tap, ...)`...) work on every
  control.

## Button

<!-- @method name="Button::__construct(int|float $x, int|float $y, string $text, Variant $variant = Variant::Secondary, Size $size = Size::Medium, int $w = 0, Shape $shape = Shape::Rounded, ?string $icon = null)" -->

A push button. Its action is `onTap()` or `on(TouchType::Tap, ...)`; it notifies no `onChange()`. `Button` is not
final: a class of your own may extend it.

| Variant | Fill | Border | Text and icon |
| --- | --- | --- | --- |
| `Variant::Primary` | `accent` | none | `onAccent` |
| `Variant::Secondary` (default) | `raised` | 1 px `borderStrong` | `text` |
| `Variant::Ghost` | none | none | `textMuted` |
| `Variant::Danger` | `danger` | none | `onAccent` |
| `Variant::Outline` | none | 1 px `accent` at 60 % | `accent` |

Pressed, the fill goes 12 % toward white; `Ghost` and `Outline`, which have no fill, show the `text` colour at 6 %.

- **Size.** The height comes from the theme: `Size::Small`, `Size::Medium` (default) and `Size::Large` are its
  `$small`, `$medium` and `$large` (40, 48 and 56 px by default). With `w: 0` the width is the text plus a padding
  on each side (16, 20 and 24 px for the three sizes), plus 20 + 8 px with an icon; with a width the content is
  centred. No text and an icon: a square, as wide as it is high.
- **Shape.** `Shape::Rounded` (default) rounds the corners by the theme's `$radius`; `Shape::Pill` by half the
  height.
- **Icon.** `$icon` is an image file, as `Image::$src` (an absolute path ending in `.sli` or `.png`, such as
  `'/app/assets/icons/back.sli'`), 20 × 20 px, drawn left of the text. Icons are **monochrome**: only their alpha
  counts, and they take the colour of the text. An icon loads as an `Image` does; one that does not load stays
  empty (with `ImageFailed`, its path in `$path`), and the button is drawn without it.

| Property | Type | Meaning |
| --- | --- | --- |
| `$text` | `string` | The text; a write lays the button out again (with `w: 0`, its width too). |
| `$variant` | `Variant` | The colours. |
| `$fill` | `?int` | The fill, `0xRRGGBB`, in place of the variant's; `null` (default): the variant's. |
| `$color` | `?int` | The colour of the text and of the icon, `0xRRGGBB`; `null` (default): the variant's. |

A write sends only what changed; a new `$variant` or `$fill` shows at once, without the 80 ms of the pressed look.

<!-- @code-block language="php" label="An icon button, disabled until a choice is made" -->
```php
$back = new Button(16, 16, '', Variant::Ghost, Size::Small, icon: '/app/assets/icons/back.sli');
$back->on(TouchType::Tap, fn () => $slate->show($home));
$next = new Button(200, 300, 'Next', Variant::Primary);
$next->enabled = false;   // drawn at 40 %, takes no touch
// ... later
$next->enabled = true;
```
<!-- @endcode-block -->

## Toggle

<!-- @method name="Toggle::__construct(int|float $x, int|float $y, bool $on = false, Size $size = Size::Medium)" -->

An on/off switch: a track with a round knob. A tap inverts `$on`: the knob and the colours slide in 250 ms, then
`onChange(bool $on)` and the callback of `changed()` run. `Toggle` is not final.

<!-- @code-block language="php" label="A toggle" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Toggle};

$slate = Slate::onboard();
$wifi = new Toggle(260, 40, on: true);
$wifi->changed(fn (bool $on) => $on ? wifi_up() : wifi_down());
$slate->show((new Scene())->add($wifi));
```
<!-- @endcode-block -->

| Size | Track | Knob |
| --- | --- | --- |
| `Size::Small` | 52 × 30 | 24 |
| `Size::Medium` (default) | 62 × 36 | 28 |
| `Size::Large` | 72 × 42 | 34 |

The knob sits 4 px from the ends of the track. On: the track is `accent` and the knob `bg`; off: the track is
`borderStrong` and the knob `textMuted`. Pressed, the track goes 12 % toward white (in the same 250 ms).

| Property | Type | Meaning |
| --- | --- | --- |
| `$on` | `bool` | The state. A write from your code slides the toggle over, with no `onChange()`. |
| `$hitPad` | `int` | 8 by default (Element's is 0): the toggle takes a touch up to 8 px around its track. |

## Slider

<!-- @method name="Slider::__construct(int|float $x, int|float $y, int $w, int|float $min = 0, int|float $max = 100, int|float $value = 0, int|float $step = 1, Size $size = Size::Medium)" -->

A value picked along a track. The area is as high as the size (the theme's `$small`, `$medium`, `$large`) and `$w`
wide; in it, a track 8 px high (`well`) filled up to the value (`accent`), and a knob of 24, 30 or 34 px (Small,
Medium, Large) in `text` with a border of 5 px in `accent`. The knob's centre runs from half a knob from the left end
to half a knob from the right end. `w: 0` is allowed: the track has no length and only the knob shows. `Slider` is
not final.

A press moves the knob under the finger, on the step; the knob then follows the finger, also outside the area (the
touch stays the slider's until the release).

- **When `onChange()` runs.** Not at the press, and not while the finger stays within 8 px of it: the knob moves, but
  the touch may still turn out to be a scroll. Once the finger is past 8 px and the touch is still the slider's,
  `onChange($value)` runs when the value changed, **at most once per frame** (frames the compositor drew): a value
  held back goes out at a later move after a frame, or at the release. At the release, a value not notified yet goes
  out first (a tap on the track notifies there), then `onCommit($value)` and the callback of `committed()`: always,
  also when the value is the one of before the press. Save to flash or send to the network in `onCommit()`, not in
  `onChange()`.
- **In a vertical `Scroll`.** The slider keeps a horizontal drag. A vertical one goes to the `Scroll` at 8 px (the
  axis of the larger movement there, as for any element in a `Scroll`): the slider gets `Cancel` and goes back to the
  value it had before the press, with no `onChange()` and no `onCommit()`.
- **Another `Cancel`** (`$enabled` set to `false`, another scene shown): before any `onChange()` of the touch it does
  the same (back to the value of before the press, no notification). After the drag has notified, the value stays:
  a value held back goes out, then `onCommit()` and the callback of `committed()` run, as at a release. A `$value`
  written by your code during the touch takes the place of the value of before the press.
- **Int or float.** `$value` (and what `onChange()` and `onCommit()` receive) is an `int` when `$min`, `$max` and
  `$step` are all `int`, a `float` otherwise.

<!-- @code-block language="php" label="A brightness slider" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Slider};

final class Brightness extends Slider
{
    public function onChange(mixed $value): void { backlight($value); }      // while dragging
    public function onCommit(int|float $value): void { save_brightness($value); }   // at the release
}

$slate = Slate::onboard();
$slate->show((new Scene())->add(new Brightness(20, 100, 320, min: 10, max: 100, value: 60, step: 5)));
```
<!-- @endcode-block -->

| Member | Type | Meaning |
| --- | --- | --- |
| `$value` | `int\|float` | The value. |
| `$min`, `$max` | `int\|float` | The range; `$min` must be less than `$max`. |
| `$step` | `int\|float` | The step: the values are `$min + k * $step`, up to the last one at or below `$max`, rounded to the decimals of `$step` and `$min` (step `0.1` gives `0.3`, not `0.30000000000000004`). |
| `$fill` | `?int` | The colour of the fill and of the knob's border, `0xRRGGBB`; `null` (default): `accent`. |
| `onCommit(int\|float $value): void` | method | The value at the release. Empty: override it. |
| `committed(?\Closure $fn): static` | method | The callback of the release (one; `null` removes it). It runs after `onCommit()`. |

A write of `$value`, `$min`, `$max` or `$step` from your code brings `$value` into the range and onto the step and
moves the knob at once, with no `onChange()`.

## Choice

<!-- @method name="Choice::__construct(int|float $x, int|float $y, array $labels, int|array $selected = 0, ChoiceStyle $style = ChoiceStyle::Segmented, int $w = 0, Size $size = Size::Medium, bool $multi = false, array $icons = [])" -->

One item (or, with `multi: true`, several) picked among `$labels`, in one of four styles. A tap on an item selects
it: its colours slide in 200 ms, then `onChange()` and the callback of `changed()` run. `Choice` is not final.

<!-- @code-block language="php" label="Segmented" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Choice, Size};

$slate = Slate::onboard();
$temp = new Choice(40, 60, ['Low', 'Mid', 'High'], 1, size: Size::Small);
$temp->changed(fn (int $i) => set_temperature($i));
$slate->show((new Scene())->add($temp));
```
<!-- @endcode-block -->

<!-- @code-block language="php" label="Chips, several at once" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Choice, ChoiceStyle, Size};

$slate = Slate::onboard();
$days = new Choice(50, 120, ['M', 'T', 'W', 'T', 'F', 'S', 'S'], [0, 2, 4], ChoiceStyle::Chips, w: 260,
                   size: Size::Small, multi: true);
$days->changed(fn (array $on) => save_days($on));   // the sorted indexes, e.g. [0, 2, 4, 5]
$slate->show((new Scene())->add($days));
```
<!-- @endcode-block -->

<!-- @code-block language="php" label="List" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Choice, ChoiceStyle};

$slate = Slate::onboard();
$lang = new Choice(60, 80, ['English', 'Italiano', 'Deutsch', 'Français'], style: ChoiceStyle::List, w: 240);
$lang->changed(fn (int $i) => set_language($i));
$slate->show((new Scene())->add($lang));
```
<!-- @endcode-block -->

<!-- @code-block language="php" label="Nav, with icons" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Choice, ChoiceStyle, Size};

$slate = Slate::onboard();
$nav = new Choice(60, 270, ['Settings', 'Buttons', 'Brew'], style: ChoiceStyle::Nav, w: 240, size: Size::Large, icons: [
    '/app/assets/icons/settings.sli', '/app/assets/icons/buttons.sli', '/app/assets/icons/brew.sli',
]);
$nav->changed(fn (int $i) => show_page($i));   // 0 Settings, 1 Buttons, 2 Brew
$slate->show((new Scene())->add($nav));
```
<!-- @endcode-block -->

Every item is as high as the size (`$small`, `$medium`, `$large` of the theme). The paddings are 16, 20 and 24 px for
`Small`, `Medium` and `Large`.

| Style | Layout | Selected | The others |
| --- | --- | --- | --- |
| `ChoiceStyle::Segmented` (default) | A `well` container (1 px `border`, radius the theme's + 4) with a padding of 4 and equal segments inside it (`h - 8` high, the theme's radius). `w: 0`: each segment as wide as the widest label plus 2 paddings; `w > 0`: the segments share `w - 8`. | `accent`, text `onAccent` | transparent, text `textMuted` |
| `ChoiceStyle::Chips` | Pills 8 px apart, each as wide as its label plus 2 paddings. `w: 0`: one row; `w > 0`: a chip that does not fit goes to the next row (rows 8 px apart). | `accent`, text `onAccent` | `raised`, 1 px `border`, text `textMuted` |
| `ChoiceStyle::List` | Rows `w` wide (required), one under the other, the label on the left. | `accent` at 12 %, 1 px `accent` at 35 %, text `accent` | transparent, text `textMuted` |
| `ChoiceStyle::Nav` | `w` (required) in equal parts: an icon (24 × 24, required) above the label in `fontSmall`. | text `accent` and a bar 3 px high at the top of the part (inset 32 % each side) that slides to it | text `textDim` |

- **Icons**: `$icons` is empty or holds one path per label (as `Image::$src`). They are monochrome: drawn in the
  colour of the item's text, which slides with it. In `Segmented`, `Chips` and `List` an icon (20 × 20) goes left of
  the label and widens the item by 28 px.
- **Pressed**: the item under the finger, from the press to the release, in 80 ms as on the other controls (a
  change of the selection takes 200 ms). An opaque fill goes 12 % toward white; a fill that is not opaque gets the
  text colour at 6 % laid over it (a transparent item shows the text colour at 6 %, the selected `List` row keeps
  its tint). A tap counts on the item that the press hit. In `Segmented`, `List` and `Nav` a touch outside the items
  (the well's padding, or the area of a larger `$hitPad`) goes to the nearest item; the gaps between chips take no
  item.
- **Nav height**: the icon (24 px), a gap of 2 and the `fontSmall` line are centred in the size's height, at least
  5 px below the top (under the bar). `Size::Small` and `Size::Medium` are tight for a bottom navigation: use
  `Size::Large`, or a theme whose `large` is taller (around 70 px).
- **One item** (default): `$selected` is the index; a tap on another item selects it and calls `onChange(int)`; a
  tap on the selected item does nothing.
- **`multi: true`**: `$selected` is the sorted list of the selected indexes (an `int` given becomes a list of one,
  `[]` selects none); each tap turns an item on or off and calls `onChange(array)` with the new list. Without
  `$selected` a multi choice starts with item 0 on (the default `0` is `[0]`): pass `[]` to start with none. In a
  `Nav` the bar marks the first selected part; with none selected it hides.

| Property | Type | Meaning |
| --- | --- | --- |
| `$selected` | `int\|array` | The selected index, or the sorted list with `multi`. A write from your code slides the colours, with no `onChange()`; one outside the labels throws and the old value stays. |

The labels, the icons, the style, the width and the size are fixed when the choice is made. The content is not
clipped to `$w`: a chip wider than `w` sits alone in its row and is wider than `$w`, a label wider than its segment
or its `Nav` part runs over its neighbours; keep the labels short or the width larger. A control has at most 127
parts: a `Choice` holds up to 41 items with icons (63 without), more throw `\Error` `Baremetal\Slate\Choice: too
many parts`.

## SettingRow

<!-- @method name="SettingRow::__construct(int|float $x, int|float $y, int $w, string $label, string $sub = '', ?Element $control = null, string $value = '')" -->

A row of a settings list, `$w` wide: a label on the left, an optional subtitle under it, an optional value on the
right and a control (a `Toggle`, a `Slider`, a `Choice`, a `Button`, any element) right-aligned, left of the value.
A 1 px divider in `border` closes it at the bottom. `SettingRow` is not final.

<!-- @code-block language="php" label="A settings list" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, SettingRow, Toggle, Slider};

$slate = Slate::onboard();
$cup = new Toggle(0, 0, on: true);
$cup->changed(fn (bool $on) => set_double_cup($on));
$volume = new Slider(0, 0, 0, max: 10, value: 6);   // w 0: the row stretches it
$level = new SettingRow(30, 152, 300, 'Volume', control: $volume, value: '6');
$volume->changed(function (int $v) use ($level) { $level->value = (string)$v; });
$slate->show((new Scene())->add(
    new SettingRow(30, 80, 300, 'Double cup', 'Two cups at once', control: $cup),
    $level,
));
```
<!-- @endcode-block -->

- **Layout**: the label (`font`, `text`) and the subtitle (`fontSmall`, `textMuted`, 2 px under the label) start at
  the row's left edge; the value (`fontMono`, `textMuted`) is right-aligned at `$w`; the control sits at the right,
  12 px left of the value when there is one, centred in the height. The height is 60, 72 with a subtitle, and at
  least the control's height + 16. The texts are centred in it.
- **The control** becomes a child of the row (it must not belong to a group yet): it gets its own touches and is
  placed by the row, which writes its `$x` and `$y` (again when a text changes).
- **A `Slider` made with `w: 0`** is stretched: its track fills the space between the label (taken at most 40 % of
  `$w` wide) and the value, 12 px from each.
- **Touch**: the row is not touchable by default (`$touchable = false`): a touch outside the control reaches what is
  under the row (the scene). With `$touchable = true`, or a callback of `on()`, the row takes the touch, with a
  pressed look (the text colour at 4 %, in 80 ms) and `onTap()`.

| Property | Type | Meaning |
| --- | --- | --- |
| `$label` | `string` | The label. A write redraws it and places the control again. |
| `$sub` | `string` | The subtitle; `''`: none (the row is 60 high). |
| `$value` | `string` | The value at the right; `''`: none. A write redraws it and places the control again. |
| `$touchable` | `bool` | `false` by default: only the control takes the touch. |

## Dialog

<!-- @method name="Dialog::__construct(string $title = '', string $text = '', int $w = 460, bool $stack = false)" -->

A modal dialog: a panel over a scrim that covers the screen. `Scene::open()` opens it above the scene,
`close()` closes it. `Dialog` is not final.

<!-- @code-block language="php" label="A confirmation" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, Dialog, Button, Variant, TouchType};

$slate = Slate::onboard();
$page = new Scene();
$slate->show($page);

$ask = new Dialog('Descale now?', "The machine runs for 20 minutes.\nKeep the drip tray in.");
$yes = new Button(0, 0, 'Descale', Variant::Primary);
$no = new Button(0, 0, 'Later', Variant::Ghost);
$yes->on(TouchType::Tap, function () use ($ask) { start_descaling(); $ask->close(); });
$no->on(TouchType::Tap, fn () => $ask->close());
$ask->buttons($no, $yes)->closed(fn () => refresh_status());
$page->open($ask);
```
<!-- @endcode-block -->

- **The scrim** covers the screen in black at 88 % and takes every touch: nothing under it gets one, nor
  `Scene::onTouch()`. With `$dismissable = true` a tap on the scrim (outside the panel) closes the dialog. While a
  dialog is open only the top one (the last opened) and what is in it take a new touch, from the moment `open()`
  returns, also in the first frames of its opening, while the scrim is still clear: a quick second tap on the
  button that opened it does not reach that button.
- **The panel** is `min($w, screen width - 32)` wide, centred: radius 26, `surface`, a 1 px border in
  `borderStrong`, a shadow (0, 30, blur 30, black at 60 %; none when shadows are not built), a padding of 26.
  From the top, 12 px apart: the title (`fontTitle`, `text`), the text (`font`, `textMuted`), the content and the
  buttons. Its height comes from what it holds and is cut to the screen height - 32.
- **The text** breaks into lines at `"\n"` only (there is no automatic wrapping): break long sentences yourself.
  Keep it to a few lines: the panel is cut to the screen height - 32 and the buttons come last, so a long text pushes
  them under the cut.
- **The content**: `add()` puts elements under the text, with coordinates from the content's top-left; it is as wide
  as the panel minus the paddings. A long content does not fit a short screen: put it in a `Scroll` of your own (its
  view sets the height the panel gives it).
- **The buttons** of `buttons(Button ...$b)` go at the bottom, in one row of equal widths (the remainder to the first
  ones), or with `stack: true` in a column at full width; the dialog sets their width and their place. Their taps
  are theirs: call `close()` from them.
- **Opening**: `Scene::open($d)` puts the dialog above the scene's elements and the dialogs open already; the scrim
  fades in and the panel rises 14 px while it fades in, in 300 ms. The panel fades element by element: each of its
  parts (background, shadow, texts, buttons) is drawn with its own opacity times the panel's, so halfway through the
  texts and buttons look washed out and what is under one part shows through it (the panel's background through a
  button); at the end it is exactly the panel at rest. (A panel faded as one image would be composed apart at every
  frame: about twice as slow.) The panel's shadow is built before the animation starts, so its first
  frame is not a long one and no step is skipped after it. Animations started in the same PHP turn right after
  `open()` (a button's pressed look, a `Slate::toast()`) start after that build too: 30 to 40 ms later at a
  dialog's first opening, not at all once its shadow is built. The touch under way ends: the element or the
  `Scroll` that holds the finger gets `Cancel` (a `Slider` with its own rules), a touch of the scene reaches
  `Scene::onTouch()` no more. A dialog is open once: `open()` of an open dialog (in any scene; also while its
  closing animation runs) throws `\Error`. A dialog is shown only with `open()`: `add()` of a `Dialog` to a scene or
  a group throws `\Error`.
- **Closing**: `close()` plays the opening backwards (300 ms), then the dialog leaves its scene, then `onClose()`
  runs, then the callback of `closed()`. While it closes, nothing in it takes a touch: a second tap on one of its
  buttons runs nothing, and a finger held on its content gets `Cancel` (the scrim still takes the touches until the
  dialog leaves). `close()` of a dialog that is not open (or is closing) does nothing. A
  dialog closing when the Slate stops leaves its scene without `onClose()` and `closed()` (no PHP runs while the
  Slate stops).
- **Dialogs and scenes**: a dialog stays with its scene. When another scene is shown it stays open (the touch under
  way gets `Cancel`, as for any `show()`) and shows again when its scene comes back. A dialog closed while its scene
  is not shown goes at once, without the animation; so does an open dialog taken out with `Scene::remove()`, and a
  dialog still closing when another scene is shown (it ends at once, so it can open in the next scene). All still
  call `onClose()` and `closed()`. While dialogs are open, `Scene::add()` keeps them on top of what it adds. A dialog
  open in a scene that is freed counts as closed (quietly) at its next `open()`.

| Member | Type | Meaning |
| --- | --- | --- |
| `$dismissable` | `bool` | A tap on the scrim closes the dialog. `false` by default. |
| `buttons(Button ...$b): static` | method | The buttons, in order (they replace the ones given before); at most 100. |
| `add(Element ...$e): static`, `remove(Element ...$e): static` | method | The content; the panel is laid out again. |
| `close(): void` | method | Closes the dialog (see above). |
| `onClose(): void` | method | Called when the dialog has closed. Empty: override it. |
| `closed(?\Closure $fn): static` | method | The callback of the close (one; `null` removes it). It runs after `onClose()`. |

`$w` and `$h` read the screen's size. The title, the text, `$w` and `$stack` are fixed when the dialog is made.
`$enabled = false` on a dialog dims it to 40 % and nothing in it takes a touch, its scrim included (a dismissable
dialog no longer closes on a tap outside the panel), but it still keeps every touch from what is under it: leave
`$enabled` alone and disable its buttons instead. `$visible = false` on an open dialog hides it and it no longer
blocks: what is under it, and `Scene::onTouch()`, take touches again (the open dialog below it, if any, is now the
top one). `close()` of a hidden dialog, and `$visible = false` on a dialog that is closing, end the close at once,
without the animation, with `onClose()` and `closed()`.

## Toasts

<!-- @method name="Slate::toast(string $text, int $ms = 2500): static" returns="static" -->

A short message above everything: a pill 48 px high (radius 24, `surface`, a 1 px border in `borderStrong`, a
shadow when shadows are built) with an `accent` dot of 8 px and the text (`font`, `text`), centred, its bottom edge
at 1/6 of the screen's height from the bottom.

<!-- @code-block language="php" label="A toast" -->
```php
$slate->toast('Saved');
$slate->toast('Water tank empty', 5000);
```
<!-- @endcode-block -->

- It rises 14 px while it fades in (300 ms), stays `$ms` (at least 500), then fades out in 300 ms and goes.
- Keep toasts short (about 20 characters at 16 px on a 360 px round screen): the pill is at most the screen's width
  - 64, and a longer text is cut.
- It is above the scene, its dialogs and a transition (it does not move with the scenes), and takes no touch.
- A new toast replaces the one shown at once. The theme is read at the call.

## Progress and status

Four controls that show a state and take no touch (`$touchable = false` by default: a touch reaches what is under
them; set it to `true`, or give a callback to `on()`, to make one touchable). A write from the code animates them;
none notifies. `$w` and `$h` read their size. `add()` and `remove()` throw, as for every control of Slate. None of
them is final.

<!-- @code-block language="php" label="A brew in progress" -->
```php
<?php
use Baremetal\Slate\{Slate, Scene, ProgressRing, ProgressBar, Spinner, StepList};

$slate = Slate::onboard();
$ring = new ProgressRing(180, 96, 56, width: 12, percent: true);
$bar = new ProgressBar(60, 184, 240, h: 6);
$bar->glow = true;
$steps = new StepList(40, 206, 280, ['Heat the water', 'Grind']);
$slate->show((new Scene())->add($ring, $bar, $steps, new Spinner(330, 30)));

$ring->value = 0.4;   // the arc slides in 200 ms, the percent shows 40
$bar->value = 0.4;
$steps->active = 1;   // "Heat the water" is done (its circle pops), "Grind" is active
```
<!-- @endcode-block -->

### ProgressRing

<!-- @method name="ProgressRing::__construct(int|float $cx, int|float $cy, int $r, float $value = 0.0, int $width = 12, bool $percent = false)" -->

A full track in `well` and an arc in `accent` with round ends, from the top (-90 degrees) clockwise for `$value` x 360
degrees. The stroke is `$width` wide, centred on the circle of radius `$r` around (`$cx`, `$cy`): the ring reaches
`$r + $width / 2` from the centre. `$x` and `$y` are the centre; the size is `2 $r + $width` square. `$clip` has no
effect on a `ProgressRing` or a `Spinner` (nothing of theirs leaves their box); `$cache` works.

- **`$value`** (0..1): a write slides the arc in 200 ms. A finite value outside 0..1 is brought into the range (a
  write of `7.0` reads back `1.0`); `INF` and `NAN` throw.
- **`$percent`**: the number `round($value x 100)` (`fontDisplay`, `text`) and a `%` (`font`, `accent`), centred
  together in the ring.
- **`$spinning`**: indeterminate. An arc of 25 % turns for ever, a turn in 900 ms; `$value` is kept but not drawn
  and the percent is hidden. Back to `false`, the arc shows the value again at once. The turn is an animation of Slate's own:
  no `TweenDone` reaches your code, and it runs only while the ring is shown.

| Property | Type | Meaning |
| --- | --- | --- |
| `$value` | `float` | The progress, 0..1. |
| `$spinning` | `bool` | Indeterminate: a turning arc. `false` by default. |
| `$color` | `?int` | The arc's colour, `0xRRGGBB`; `null`: `accent`. |

### ProgressBar

<!-- @method name="ProgressBar::__construct(int|float $x, int|float $y, int $w, float $value = 0.0, int $h = 4)" -->

A track `$w` x `$h` in `well` and a fill in `accent`, `$value` x `$w` wide, both with the radius `$h / 2`. A write of
`$value` slides the fill in 200 ms (the same range rules as `ProgressRing`).

- **`$glow`**: a glow around the fill, a shadow of the fill's colour (0, 0, blur 10). A firmware built without
  shadows (`slate-effects`, see [Effects and build options](./effects-and-options.md)) draws none: the bar looks
  the same without it.

| Property | Type | Meaning |
| --- | --- | --- |
| `$value` | `float` | The progress, 0..1. |
| `$glow` | `bool` | The glow of the fill. `false` by default. |
| `$color` | `?int` | The fill's colour, `0xRRGGBB`; `null`: `accent`. |

### Spinner

<!-- @method name="Spinner::__construct(int|float $cx, int|float $cy, int $r = 12)" -->

A busy indicator: an arc of 25 %, 3 px wide with round ends, in `accent`, on the circle of radius `$r` around (`$cx`,
`$cy`), that turns for ever (a turn in 900 ms). `$color` (`?int`) changes its colour. It turns only while it is shown:
a spinner in a scene that is not shown, or with `$visible = false` on itself or on a group it is in, costs nothing
(it goes on from where it stopped when it is visible again), and while it turns only its own small area is drawn
again (no `TweenDone` either). `$x` and `$y` are the centre; the size is `2 $r + 3` square.

### StepList

<!-- @method name="StepList::__construct(int|float $x, int|float $y, int $w, array $steps, int $active = 0)" -->

The steps of a process, one row per text of `$steps`: rows of `$w` x 48, 8 px apart, radius 14, a circle of 24 px at
the left and the text (`font`) 48 px from the left edge.

- **Done** (before `$active`): the circle in `success` at 15 % with a check in `success`; the text in `textMuted`.
- **Active** (`$active`): a small spinner in the circle and the row's border in `accent`; the text in `text`.
- **To do**: a 2 px ring in `borderStrong`; the text in `textDim`.

A write of `$active` (0..`count($steps)`) moves on: the colours fade in 200 ms and the steps that become done pop
(their circle from 40 % to 108 % of its size in 260 ms, then back to 100 % in 140 ms). `$active = count($steps)`: all
done, no spinner. `$steps` is read-only; a list holds at most 21 steps. The height is `56 count($steps) - 8`.

| Property | Type | Meaning |
| --- | --- | --- |
| `$active` | `int` | The active step, 0..`count($steps)`. |
| `$steps` | `array` | The texts (read-only). |

## A control of your own

A class that extends `Control` directly builds its parts with `add()` (any element: `RoundRect`, `Text`, `Image`,
`Arc`...) and gets `$enabled`, the theme, the notifications and the pressed hook. It calls `notify()` for the
user's gestures, and only for them.

<!-- @code-block language="php" label="Five beans: the strength of a coffee" -->
```php
<?php
use Baremetal\Slate\{Control, RoundRect, Touch};

final class Beans extends Control
{
    public int $level = 0;
    /** @var RoundRect[] */
    private array $beans = [];

    public function __construct(int $x, int $y)
    {
        parent::__construct($x, $y);
        for ($i = 0; $i < 5; $i++) {
            $this->beans[] = new RoundRect($i * 16, 0, 14, 20, radius: 4, fill: $this->theme->well);
        }
        $this->add(...$this->beans);
    }

    /** A tap on bean i sets the level to i + 1. */
    public function onTap(Touch $t): void
    {
        $this->toLevel(intdiv($t->localX, 16) + 1);
        $this->notify($this->level);   // the user's gesture: onChange(), then changed()
    }

    /** From the code: drawn, not notified. */
    public function toLevel(int $level): void
    {
        $this->level = $level;
        foreach ($this->beans as $i => $bean) {
            $bean->fill = $i < $level ? $this->theme->accent : $this->theme->well;
        }
    }

    protected function pressed(bool $down): void
    {
        $this->opacity = $down ? 0.8 : 1.0;
    }
}

$beans = new Beans(40, 120);
$beans->changed(fn (int $level) => $machine->strength($level));
```
<!-- @endcode-block -->

A part that is hard to draw with shapes can be an image; the pressed look can be a second image over it, shown by
`pressed()`:

<!-- @code-block language="php" label="A card made of two images" -->
```php
<?php
use Baremetal\Slate\{Control, Image, Text, Align};

final class DrinkCard extends Control
{
    private Image $down;

    public function __construct(int $x, int $y, string $name, string $art, string $artPressed)
    {
        parent::__construct($x, $y);
        $this->add(
            new Image(0, 0, $art),
            $this->down = new Image(0, 0, $artPressed),
            new Text(75, 100, $name, color: $this->theme->text, align: Align::Center),
        );
        $this->down->visible = false;
    }

    protected function pressed(bool $down): void
    {
        $this->down->visible = $down;
    }
}
```
<!-- @endcode-block -->

## Errors

| When | Throws | Message |
| --- | --- | --- |
| A control made without a running Slate | `\Error` | `Baremetal\Slate\Button needs a running Slate: create the Slate before its controls` (the class of the object) |
| `add()` or `remove()` on Slate's controls | `\Error` | `Baremetal\Slate\Button: a control builds its own parts` |
| A write of `$w` or `$h` of Slate's controls | `\Error` | `Cannot modify Baremetal\Slate\Button::$w: the size of a control comes from its constructor and the theme` |
| `Button`: `$w` outside 0..32767 | `\ValueError` | `Baremetal\Slate\Button::$w must be between 0 and 32767` |
| `Button`: `$icon` not an absolute `.sli` or `.png` path | `\ValueError` | `Baremetal\Slate\Button::$icon must be an absolute path ending in .sli or .png` |
| `Button`: a `.png` icon while png is not built | `\ValueError` | `png is not built in: add it to slate-images in composer.json` |
| `Button`: `$fill` or `$color` outside `0..0xFFFFFF` | `\ValueError` | `Baremetal\Slate\Button::$fill must be a color, 0xRRGGBB` |
| `Slider`: `$w` outside 0..32767 | `\ValueError` | `Baremetal\Slate\Slider::$w must be between 0 and 32767` |
| `Slider`: `$min`, `$max`, `$value` or `$step` not finite (`INF`, `NAN`) | `\ValueError` | `Baremetal\Slate\Slider::$value must be a finite number` (the name given) |
| `Slider`: `$min` not less than `$max` (constructor or write; the old value stays) | `\ValueError` | `Baremetal\Slate\Slider::$min must be less than $max` |
| `Slider`: `$step` 0 or less | `\ValueError` | `Baremetal\Slate\Slider::$step must be greater than 0` |
| `Slider`: `$fill` outside `0..0xFFFFFF` | `\ValueError` | `Baremetal\Slate\Slider::$fill must be a color, 0xRRGGBB` |
| `Choice`: `$labels` empty or not all strings | `\ValueError` | `Baremetal\Slate\Choice::$labels must be a non-empty list of strings` |
| `Choice`: `$selected` outside the labels (constructor or write; the old value stays) | `\ValueError` | `Baremetal\Slate\Choice::$selected must be between 0 and 2` (with `multi`: `...::$selected must be a list of indexes between 0 and 2`) |
| `Choice`: `$icons` neither empty nor one per label | `\ValueError` | `Baremetal\Slate\Choice::$icons must be empty or hold one path per label` |
| `Choice`: `$w` outside 0..32767 | `\ValueError` | `Baremetal\Slate\Choice::$w must be between 0 and 32767` |
| `Choice`: `List` or `Nav` with `w: 0` | `\ValueError` | `Baremetal\Slate\Choice: the List and Nav styles need w greater than 0` |
| `Choice`: `Nav` without icons | `\ValueError` | `Baremetal\Slate\Choice: the Nav style needs an icon per label` |
| `Choice`: more than 41 items with icons (63 without) | `\Error` | `Baremetal\Slate\Choice: too many parts` |
| `Choice`: an icon not an absolute `.sli` or `.png` path | `\ValueError` | `Baremetal\Slate\Choice::$icons must hold absolute paths ending in .sli or .png` |
| `SettingRow`: `$w` outside 1..32767 | `\ValueError` | `Baremetal\Slate\SettingRow::$w must be between 1 and 32767` |
| `SettingRow`: a `$control` that belongs to a group or a scene | `\LogicException` | `Baremetal\Slate\Toggle already belongs to a group: remove() it from there first` |
| `Dialog`: `$w` outside 1..32767 | `\ValueError` | `Baremetal\Slate\Dialog::$w must be between 1 and 32767` |
| `Dialog`: `$text` with more than 100 lines | `\ValueError` | `Baremetal\Slate\Dialog::$text must hold at most 100 lines` |
| `Scene::open()` of a dialog open already (in any scene, also while it closes) | `\Error` | `Baremetal\Slate\Dialog is already open` (the class of the object) |
| `add()` of a `Dialog` to a scene or a group | `\Error` | `Baremetal\Slate\Dialog is opened with Scene::open(), not added` |
| `Dialog::buttons()` with more than 100 buttons | `\ValueError` | `Baremetal\Slate\Dialog::buttons(): at most 100 buttons` |
| `ProgressRing`: `$r` or `$width` outside 1..32767 | `\ValueError` | `Baremetal\Slate\ProgressRing::$r must be between 1 and 32767` (the name given) |
| `ProgressBar`: `$w` or `$h` outside 1..32767 | `\ValueError` | `Baremetal\Slate\ProgressBar::$w must be between 1 and 32767` (the name given) |
| `ProgressRing`, `ProgressBar`: `$value` not finite (`INF`, `NAN`; constructor or write) | `\ValueError` | `Baremetal\Slate\ProgressRing::$value must be a finite number` |
| `ProgressRing`, `ProgressBar`, `Spinner`: `$color` outside `0..0xFFFFFF` | `\ValueError` | `Baremetal\Slate\Spinner::$color must be a color, 0xRRGGBB` |
| `Spinner`: `$r` outside 1..32767 | `\ValueError` | `Baremetal\Slate\Spinner::$r must be between 1 and 32767` |
| `StepList`: `$w` outside 1..32767 | `\ValueError` | `Baremetal\Slate\StepList::$w must be between 1 and 32767` |
| `StepList`: `$steps` empty, not a list or not all strings | `\ValueError` | `Baremetal\Slate\StepList::$steps must be a non-empty list of strings` |
| `StepList`: more than 21 steps | `\ValueError` | `Baremetal\Slate\StepList::$steps must hold at most 21 steps` |
| `StepList`: `$active` outside 0..`count($steps)` (constructor or write; the old value stays) | `\ValueError` | `Baremetal\Slate\StepList::$active must be between 0 and 3` |
| A write of `StepList::$steps` | `\Error` | `Cannot modify readonly property Baremetal\Slate\StepList::$steps` |
| `Slate::toast()` with `$ms` less than 500 | `\ValueError` | `Baremetal\Slate\Slate::toast(): ms must be at least 500` |

The controls are built with the `slate-controls` build option (default `true`): see
[Effects and build options](./effects-and-options.md#build-options).
