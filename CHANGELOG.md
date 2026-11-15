# Changelog

## 1.3.0

- Added: themes. `final class Theme` holds the look of the controls: 14 colours (`$bg`, `$surface`, `$raised`,
  `$well`, `$border`, `$borderStrong`, `$text`, `$textMuted`, `$textDim`, `$accent`, `$onAccent`, `$danger`,
  `$info`, `$success`, `0xRRGGBB`), `$radius` (0..64, the radius of `Shape::Rounded`), `$small`, `$medium`, `$large`
  (24..128, the heights of the sizes) and five fonts of the firmware (`$font`, `$fontSmall`, `$fontTitle`,
  `$fontMono`, `$fontDisplay`), all `public readonly`, set by name in the constructor (the defaults are a neutral
  dark theme). `Slate::theme(Theme $theme): static` sets it before the first `show()` (after it: `\Error` `the
  theme is set before the first show()`); `Slate::$theme` (`public readonly Theme`) reads it, `new Theme()` until
  `theme()`. A control reads the theme when it is made. The enums of the controls: `Size` (`Small`, `Medium`,
  `Large`), `Variant` (`Primary`, `Secondary`, `Ghost`, `Danger`, `Outline`), `Shape` (`Pill`, `Rounded`) and
  `ChoiceStyle` (`Segmented`, `Chips`, `List`, `Nav`). `\ValueError` `Baremetal\Slate\Theme::$<token> must be a
  color, 0xRRGGBB`, `...::$radius must be between 0 and 64`, `...::$<small|medium|large> must be between 24 and 128`,
  `...::$<font> must be a font of the firmware: unknown font '<name>' (available: ...)`.
- Added: controls. `abstract class Control extends Group`: one PHP object whose parts live in the compositor without
  a PHP object each; `public bool $touchable = true`, `public bool $enabled = true` (`false`: drawn at 0.4 times
  `$opacity`, no new touch, a touch under way gets `Cancel`), `protected readonly Theme $theme` (the running Slate's
  theme when it is made), `onChange(mixed $value): void` (empty) and `changed(?\Closure $fn): static` (notified for
  the user's gestures only: method first, then the callback), `protected notify(mixed $value): void`, `protected
  pressed(bool $down): void` (`true` at `Press`, `false` at `Release` and `Cancel`), `add()` / `remove()` for the
  controls of your own (a class that extends `Control` directly). A control needs a running Slate when it is made
  (`\Error` `<Class> needs a running Slate: create the Slate before its controls`); `add()` / `remove()` on Slate's
  controls throw `\Error` `<Class>: a control builds its own parts`; their `$w` and `$h` read the size laid out and
  cannot be written (`\Error` `Cannot modify <Class>::$w: the size of a control comes from its constructor and the
  theme`). `class Button extends Control`: `new Button(int|float $x, int|float $y, string $text, Variant $variant =
  Variant::Secondary, Size $size = Size::Medium, int $w = 0, Shape $shape = Shape::Rounded, ?string $icon = null)`,
  `public string $text`, `public Variant $variant`, `public ?int $fill = null`, `public ?int $color = null`; the
  height from the theme, the width given or the text plus paddings (16, 20, 24 px) and the icon (20 + 8 px), a square
  with an icon and no text; the pressed look (the fill 12 % toward white in 80 ms; `Ghost` and `Outline`: the text
  colour at 6 %); a monochrome icon (an image file as `Image::$src`, drawn in the text colour; one that does not
  load stays empty, with `ImageFailed`). `\ValueError` `Baremetal\Slate\Button::$w must be between 0 and 32767`,
  `...::$icon must be an absolute path ending in .sli or .png`, `...::$fill must be a color, 0xRRGGBB` (and
  `$color`).
- Added: `Toggle` (an on/off switch) and `Slider` (`extends Control`, not final). `new Toggle(int|float $x, int|float
  $y, bool $on = false, Size $size = Size::Medium)`: a track (52 × 30, 62 × 36, 72 × 42) and a knob (24, 28, 34, 4 px
  from the ends); on: `accent` track and `bg` knob, off: `borderStrong` and `textMuted`; a tap inverts `public bool
  $on` (knob and colours slide in 250 ms) and notifies `onChange(bool)`; a write of `$on` from the code slides without
  a notification; `public int $hitPad = 8`. `new Slider(int|float $x, int|float $y, int $w, int|float $min = 0,
  int|float $max = 100, int|float $value = 0, int|float $step = 1, Size $size = Size::Medium)`: an area as high as the
  size, a track 8 px high (`well`) filled to the value (`accent` or `public ?int $fill`), a knob of 24, 30, 34 px; the
  press moves the knob under the finger, on the step, and it follows the finger; `onChange()` only once the finger is
  past 8 px (or at the release), at most once per frame of the compositor; `onCommit(int|float $value): void` and
  `committed(?\Closure $fn): static` at the release, always; in a vertical `Scroll` a vertical drag goes to the
  `Scroll` (the slider gets `Cancel` and goes back to its value of before the press, no notification). Another
  `Cancel` (`$enabled` false, another scene) does the same before any `onChange()`; after one, the value stays and
  `onCommit()` runs. `public int|float $value`, `$min`, `$max`, `$step`: a write brings `$value` into the range and
  onto the step (rounded to the decimals of `$step` and `$min`) without a notification; `$value` is an `int` when
  `$min`, `$max` and `$step` are all `int`. `\ValueError` `Baremetal\Slate\Slider::$w must be between 0 and 32767`,
  `...::$min must be less than $max`, `...::$step must be greater than 0`, `...::$<value|min|max|step> must be a
  finite number`, `...::$fill must be a color, 0xRRGGBB`.
- Added: `Choice` (`extends Control`, not final). `new Choice(int|float $x, int|float $y, array $labels, int|array
  $selected = 0, ChoiceStyle $style = ChoiceStyle::Segmented, int $w = 0, Size $size = Size::Medium, bool $multi =
  false, array $icons = [])`: one item (or several, `multi: true`) picked among the labels, in four styles:
  `Segmented` (equal segments in a `well` container; selected `accent` + `onAccent`), `Chips` (pills 8 px apart that
  wrap within `w > 0`; selected `accent`, the others `raised` with a `border`), `List` (rows `w` wide; selected
  `accent` at 12 % with a border at 35 % and `accent` text) and `Nav` (equal parts of `w`, an icon above a
  `fontSmall` label; selected `accent` with a 3 px bar that slides to it, the others `textDim`). `$icons`: one path
  per label, monochrome icons drawn in the item's text colour (required for `Nav`). The colours slide in 200 ms; the
  item under the finger has the pressed look (80 ms; a fill that is not opaque gets `text` at 6 % laid over it). A
  tap selects an item and notifies `onChange(int)` (a tap on the selected one does nothing); with `multi` (pass `[]`
  to start with none) each tap turns an item on or off and `onChange()` gets the sorted list.
  `public int|array $selected`: a write from the code slides without a notification. `\ValueError`
  `Baremetal\Slate\Choice::$labels must be a non-empty list of strings`, `...::$selected must be between 0 and
  <n-1>` (`multi`: `...::$selected must be a list of indexes between 0 and <n-1>`), `...::$icons must be empty or hold
  one path per label`, `...::$w must be between 0 and 32767`, `...: the List and Nav styles need w greater than 0`,
  `...: the Nav style needs an icon per label`, `...::$icons must hold absolute paths ending in .sli or .png`.
- Added: `SettingRow`, `Dialog`, `Scene::open()`, `Slate::toast()`. `new SettingRow(int|float $x, int|float $y, int
  $w, string $label, string $sub = '', ?Element $control = null, string $value = '')`: a row of a settings list, 60
  high (72 with `$sub`, at least the control + 16), the label (`font`) and the subtitle (`fontSmall`, `textMuted`) on
  the left, `$value` (`fontMono`, `textMuted`) right-aligned, the control (a child of the row, with its own touches)
  at the right, left of the value, and a divider in `border`; a `Slider` made with `w: 0` is stretched between the
  label (at most 40 % of `$w`) and the value. `public string $label`, `$sub`, `$value` (a write lays the row out
  again); `public bool $touchable = false` (touchable: the pressed look, `text` at 4 %, and `onTap()`). `new
  Dialog(string $title = '', string $text = '', int $w = 460, bool $stack = false)`: a scrim (black at 88 %) that
  takes every touch, and a panel `min($w, screen width - 32)` wide (radius 26, `surface`, `borderStrong`, a shadow
  when shadows are built, padding 26) with the title (`fontTitle`), the text (`font`, `textMuted`; lines at `"\n"`
  only), the content of `add()` and the buttons of `buttons(Button ...$b): static` (one row of equal widths, or a
  column with `$stack`; the dialog sets their width and place), cut to the screen height - 32. `Scene::open(Dialog
  $d): static` opens it above the scene (its shadow built first, then the scrim fades in and the panel, opaque,
  rises 14 px, 300 ms);
  `close(): void` plays it backwards, then the dialog leaves the scene, then `onClose(): void` and the callback of
  `closed(?\Closure $fn): static`; `public bool $dismissable = false` (a tap on the scrim closes it). While a dialog
  is open only the top one and what is in it take a new touch, from `open()` on (also in the first frames, while
  the scrim is still clear); while it closes nothing in it takes a touch (a finger held on it gets `Cancel`), the
  scrim still does; `$enabled = false` on a dialog leaves its scrim without touches but still blocks what is under
  it; a dialog with `$visible = false` blocks nothing, and its close (or a close under way when it is hidden) ends
  at once. A dialog stays
  with its scene across `show()`; closed while its scene is not shown, or taken out with `Scene::remove()`, it goes
  at once and still calls `onClose()`, as does one still closing when its scene is left; `Scene::open()` ends the
  touch under way (`Cancel`); `add()` of a `Dialog` throws `\Error` `<Class> is opened with Scene::open(), not
  added`; `Scene::add()` keeps the open dialogs on top. `Slate::toast(string $text, int
  $ms = 2500): static`: a pill (48 high, `surface`, an `accent` dot, the text) above everything, also a transition,
  in a top layer of the compositor; it rises and fades in (300 ms), stays `$ms`, fades out (300 ms); a new toast
  replaces the one shown; it takes no touch; it is at most the screen's width - 64 (a longer text is cut).
  `\ValueError` `Baremetal\Slate\SettingRow::$w must be between 1 and 32767` (and `Dialog::$w`),
  `Baremetal\Slate\Dialog::$text must hold at most 100 lines`, `Baremetal\Slate\Slate::toast(): ms must be at least
  500`; `\Error` `Baremetal\Slate\Dialog is already open`. The controls' animations
  give no `TweenDone`; Slate's internal `HoldDue` now also wakes the PHP task at the end of a dialog's close (no
  more watches of ext-events: with the controls built, a Slate without a touch device takes the one of `HoldDue`).
- Added: `ProgressRing`, `ProgressBar`, `Spinner`, `StepList` (`extends Control`, not final; not touchable by
  default, `public bool $touchable = false`). `new ProgressRing(int|float $cx, int|float $cy, int $r, float $value =
  0.0, int $width = 12, bool $percent = false)`: a track in `well` and an arc in `accent` with round ends from the top
  for `$value` x 360 degrees, sliding in 200 ms; `public float $value` (0..1, a finite value outside is brought into
  the range), `public bool $spinning = false` (an arc of 25 % that turns, 900 ms a turn), `public ?int $color = null`;
  `$percent` shows `round($value x 100)` (`fontDisplay`) and `%` (`font`, `accent`) in the middle. `new
  ProgressBar(int|float $x, int|float $y, int $w, float $value = 0.0, int $h = 4)`: a track in `well` and a fill in
  `accent` (radius `h / 2`) sliding in 200 ms; `public float $value`, `public bool $glow = false` (a shadow of the
  fill's colour, blur 10; none when shadows are not built), `public ?int $color = null`. `new Spinner(int|float $cx,
  int|float $cy, int $r = 12)`: an arc of 25 %, 3 px, turning (900 ms a turn) while it is shown; `public ?int $color
  = null`. `new StepList(int|float $x, int|float $y, int $w, array $steps, int $active = 0)`: one row per step (48
  high, 8 apart), done (a `success` circle with a check, `textMuted`), active (a spinner, an `accent` border, `text`),
  to do (a `borderStrong` ring, `textDim`); `public int $active` (0..`count($steps)`; the steps that become done
  pop), `public readonly array $steps` (at most 21). The turns and the pops give no `TweenDone`. `\ValueError`
  `<Class>::$r must be between 1 and 32767` (`ProgressRing`, `Spinner`), `...::$width must be between 1 and 32767`,
  `...::$w must be between 1 and 32767` (`ProgressBar`, `StepList`), `...::$h must be between 1 and 32767`,
  `...::$value must be a finite number`, `...::$color must be a color, 0xRRGGBB`, `...::$steps must be a non-empty list
  of strings`, `...::$steps must hold at most 21 steps`, `...::$active must be between 0 and <n>`.
- Changed: a tween of an element with `$visible = false`, or inside a group with `$visible = false`, waits (its
  time stopped, its `TweenDone` and the tweens chained after it too) and goes on from where it was when the element
  is visible again, as one of a scene not shown does; only `$visible` counts (a fade-in from `$opacity` 0 runs). A
  hidden `Spinner` no longer keeps the compositor waking at every refresh. A tween that draws nothing (its element
  out of a `Scroll`'s view, or at `$opacity` 0) no longer makes the touch table be built again at every refresh.
- Fixed: on a panel that refreshes top to bottom, a frame that changes more than half of the screen also races the
  panel's refresh (no tearing) when its last band can still be written behind the refresh before the next TE pulse
  (other refresh orders: as before). A `Scroll` whose view is just over half
  of the screen (280 x 232 on the 360 x 360 panel) tore while it scrolled: rows looked cut or squeezed for one
  refresh. A larger change that reaches the bottom of the scan (a whole screen) is sent as before.
- Fixed: an element taken out of the scene (a `Dialog` that closes) kept its caches out of reach: they stayed in the
  cache budget until it was freed, so a later shadow, blur or cached group could find no room (the second dialog of
  the example was drawn without its shadow, and tried again at every frame). Such caches are still kept (a dialog
  opened again builds nothing), but they make room first when a cache does not fit, before those of the scenes left.
- Changed: a `Dialog`'s panel fades element by element: each of its parts is drawn with its own opacity times the
  panel's (an internal flat group), instead of being composed apart as one image at every frame;
  halfway through, texts and buttons look washed out and the panel's background shows through them. Its shadow is
  blur 30 (was 80), and it is built before the opening starts (`SLATE_CMD_PREPARE`: the tweens pushed after it start
  once it is built), so the first frame is not a long one and the animation skips no step after it. On the
  360 x 360 example: frames of 34 to 49 ms during the 300 ms (were 93 to 101 ms), 7 to 9 of them (were 4 or 5), and
  the first opening no longer has a frame of 160 to 173 ms: its shadow takes 31 to 36 ms before the animation
  starts.
- Changed: the blur of a shadow 385 to 512 px wide or high (was up to 384), such as the mask of a `Dialog` of the
  default width on a 360 x 360 screen (388 px), takes the faster path that reads and writes the image once per
  direction (about half the time with the image in PSRAM).
- Added: the `slate-controls` build option (`bool`, default `true`). With `false` the theme, its enums, `Control`,
  the controls, `Slate::theme()`, `Slate::$theme`, `Slate::toast()` and `Scene::open()` are not built.
- Docs: `docs/usage/controls.md` and `docs/usage/theme.md` (new), `docs/usage/performance.md` (Controls: the touch
  table, tinted icons, dialogs, latency, memory per control, firmware), `effects-and-options.md`, `overview.md`;
  IDE stub. The `06-display/slate-controls` example shows the controls and a theme on the round 360 x 360 panel.

## 1.2.0

- Added: touch on elements. An element takes the finger when `Element::$touchable` is `true` (`bool`, default
  `false`; a component of your own declares `public bool $touchable = true;`) or when it has a callback:
  `Element::on(TouchType $type, \Closure $fn): static` (one per kind, a new one replaces the old one) and
  `Element::off(TouchType $type): static`. The kinds reach the element's methods `onPress()`, `onMove()`,
  `onRelease()`, `onTap()`, `onHold()`, `onCancel()` (public, `(Touch $t): void`, empty, for subclasses to override)
  and then its callback. The area is what the finger sees in the frame shown (a group with its children, a clipping
  group its whole clip box), grown by `Element::$hitPad` (0..64 px, default 0) and cut by the clips of its
  ancestors; the touchable drawn on top wins, and children that take no touch are part of their touchable ancestor.
  `Press` captures the finger (`Move` and `Release` come wherever it goes); `Tap` follows `Release` when the finger
  stayed within 8 px of the press, no `Hold` came and it came up on the area; `Hold` comes once after
  `Element::$holdMs` (0..10000 ms, default 500; 0: never) without a move past 8 px, and then no `Tap`; `Cancel` (no
  `Release`, no `Tap`) when a `Scroll` it is in takes the finger, the element or an ancestor is removed or hidden,
  the element stops taking touches, or `Slate::show()` shows another scene or starts a transition (a cause inside a
  touch handler: right after it returns; `show()`: at once, before `onLeave()`). An element hidden (also at
  `$opacity` 0), made not touchable or removed takes no new touch at once, before the next frame. A `Scroll` takes
  the touches by itself (`Scroll::$touchable` defaults to `true`), with the state machine of `Scroll::touch()`: a
  press on its view drives it; a touchable element inside it that moves more than 8 px before a `Hold`, on the axis
  the scroll can move (the larger movement), gets `Cancel` and the scroll takes the touch from the press point and
  time, following the finger from the point past 8 px without a jump (no axis it can move: the element keeps it); a
  press while it moves is a catch (the element under the finger gets nothing); scrolls inside scrolls: the innermost
  that can move in that direction. A scroll's touch that ends without its `Released` (the causes of `Cancel`, or a
  new `Pressed` first) stops its drag without speed (`ScrollEnded` follows once a drag had started). A scroll hidden
  during a drag routed by hand with `touch()` now ends it as well (at its `Released`: before, it stayed armed and the
  next press was a catch). `Scroll::touch()` stays for routing by
  hand (`$touchable = false`, or touches that hit nothing) and does nothing (returns `false`) for a touch Slate
  routes to that scroll. `final class Touch`, made by Slate only (private constructor; read-only
  `TouchType $type`, `Element $element`, `int $x`, `$y`, `$localX`, `$localY` from the element's origin on the
  screen, `int|float $time` as `Event::$time`: ns on the base of `hrtime(true)`, a float on 32-bit PHP), `enum
  TouchType` (`Press`, `Move`, `Release`, `Tap`, `Hold`, `Cancel`). **Changed**: `Scene::onTouch()` now
  gets only the touches whose `Pressed` hits no touchable element (a scene without touchable elements gets them all,
  as before); since a `Scroll` is touchable by default, the touches that hit a scroll no longer reach
  `Scene::onTouch()` (set `$touchable = false` on it to keep routing them by hand with `touch()`); nor does the
  `Gesture` of a touch an element or a scroll took, which the touch controller may report after the `Released`.
  Touches stay ignored during a transition. Slate takes one more of ext-events' eight watches (six: four for the touch events,
  one for the `Hold` timer through the internal coalescing event `HoldDue`, one for `TransitionDone`).
  `\ValueError` `Baremetal\Slate\<Class>::$hitPad must be between 0 and 64`, `...::$holdMs must be between 0 and
  10000`, `...::$<name> cannot be animated` for the three properties; `\RuntimeException` `Slate: cannot take the
  touch: the table of ext-events' watches is full`. A scene without touchable elements does no more work. Engine
  side: the properties `SLATE_PROP_TOUCHABLE` and `SLATE_PROP_HIT_PAD`, a table of the touchables' boxes published
  after each frame that changed something (`src/hit.h`; none with zero touchables), `slate_engine_set_hit_pub`, the
  stats `hit_builds`, `hit_entries`, `hit_nomem`, and the scroll command `SLATE_SCROLL_OP_CANCEL` (a release without
  velocity). Docs: `docs/usage/touch.md`, `docs/usage/scroll.md` (touch); IDE stub. The `06-display/slate-scroll`
  example uses touchable rows instead of its own hit maths.
- Added: animations, run by the compositor at the pace of the panel (no PHP timer). `Element::animate(array $props,
  int $ms = 300, Ease $ease = Ease::OutCubic, int $delay = 0, int $repeat = 0, bool $yoyo = false): Tween` moves
  properties from their values on the screen to the ones given; the properties read their final values at once.
  Animatable: `x`, `y`, `opacity` of every element, `Group` `w`, `h`, `blur`, `RoundRect` `w`, `h`, `radius`,
  `border`, `fill`, `borderColor`, `Circle` `r`, `border`, `fill`, `borderColor`, `Arc` `r`, `from`, `to`, `width`,
  `color`, `Line` `x1`, `y1`, `width`, `color`, `Text` `color` (colours channel by channel). `final class Tween`:
  read-only `$done`, `stop(bool $toEnd = false)` (with the chain after it, no event), `wait(int $timeout_ms = 5000)`
  (hands the changes over, delivers no other event; false at once for a stopped tween or `repeat: -1`),
  `then(Element $e, array $props, int $ms = 300, Ease $ease = Ease::OutCubic): Tween` (starts at its exact end).
  `Element::ease(array $props, int $ms = 300, Ease $ease = Ease::OutCubic): static`: writes of those properties slide
  (`ease([])` removes it). The event `TweenDone` (`$tween`, final), once per tween that ends by itself, never for
  `repeat: -1` nor after a stop (unless the tween ended in the frame the stop arrived); a `$done` whose event was dropped (a full event pool, `init-loop`) is set at the
  return to the event loop. The enum `Ease` (`Linear`, `InQuad`, `OutQuad`, `InOutQuad`, `InCubic`, `OutCubic`,
  `InOutCubic`, `OutBack`, `OutBounce`, `OutElastic`). Tweens of a scene that is not shown wait for it; `remove()`
  of an element stops its tweens (a `remove()` and `add()` in one delivery keeps them), and so does the end of its
  group or scene (a page dropped with its animations running leaks nothing); the tween of an element not drawn yet
  (made in `build()`, or before `add()`) waits and starts when it is shown, `then()` after it too. Everything is
  checked before anything changes, with
  `\ValueError` `Baremetal\Slate\<Class>::$<name> cannot be animated`, `...::$fill cannot be animated: it is a
  gradient` (likewise `$color`), `...::$fill cannot be animated: it is null` (likewise `$borderColor`),
  `ms must be at least 1`, `delay must be 0 or more`, `repeat must be -1 or more`,
  `props must name at least one property`, or the property's own range message. A write of the value a property
  already holds is sent all the same on an element that was animated (the screen may show another value after a
  stop). Docs: `docs/usage/animations.md`; IDE stub 1.2.0.
- Added: scroll. `class Scroll extends Group` (not final): a view `(0, 0, $w, $h)`, always clipped, over its children
  (the content, in content coordinates), drawn shifted by the scroll position. `Scroll::__construct(int|float $x = 0,
  int|float $y = 0, int|float $w = 1, int|float $h = 1, ScrollAxis $axis = ScrollAxis::Both, bool $bounce = true, int
  $snapX = 0, int $snapY = 0, int $snapMax = 0, bool $indicators = true, int $contentW = 0, int $contentH = 0, float
  $opacity = 1.0, bool $cache = false)`. Properties: `$contentW`, `$contentH` (0..32767; 0: the box of the children from
  (0, 0), kept up to date), `$axis`, `$bounce`, `$snapX`, `$snapY` (0..32767, 0: none), `$snapMax` (0..32767, 0: no
  limit; at most that many snap steps per release, counted in the direction of the fling from the step at or behind the
  release position, per axis with a snap step, never back against the fling, the nearest step under 20 px/s: `snapMax:
  1` moves a carousel one card per fling), `$indicators`, `$scrollX`, `$scrollY` (read: the position last drawn, the
  value written until it is drawn; written: a jump kept within `0..max(0, content - view)`, which stops any motion and
  ends a drag), `$clip` always `true`, `$w`, `$h` 1..32767; `$cache` keeps the whole content as one image copied at each
  position (composed directly when it does not fit the cache; with `slate-debug` the console says so once).
  `touch(\Baremetal\Input\TouchEvent $e): bool`: touches routed by hand (usually from `Scene::onTouch()`; a scroll
  also takes them by itself, see touch on elements);
  `true` when the touch is a drag of the scroll (past 8 px from the press on the axis allowed, with `Both` the distance;
  the Released that ends it) or a catch (a press inside the view that stopped its motion, with every Moved and the
  Released of that touch); `false` for a press outside the view, a tap, a `Gesture` and a scroll not in the scene shown.
  The compositor runs the drag (following the finger from the threshold point), the release velocity of the last 80 ms
  (at most 4000 px/s), the inertia (325 ms time constant), the rubber band past an edge (`d * (1 - 1 / (over * 0.55 / d
  + 1))`, at most half the view) with a critical spring back, the snap to a step (`OutCubic`, 150 to 600 ms; the end
  counts as a step) and the bars (3 px, 2 px from the right and bottom edges, `view² / content` long, at least 16 px;
  shown while the content moves, then 500 ms, then faded in 200 ms), at the frame's time; a motion pauses while its
  scene is not shown and goes on during a transition; a kept scene keeps its position. `scrollTo(int|float $x, int|float
  $y, int $ms = 300, Ease $ease = Ease::OutCubic): Tween` (`ms` 0: a jump, the Tween done at once; a press, a write of
  the position or a new set-up stops it without `TweenDone`). The enum `ScrollAxis` (`Both`, `Vertical`, `Horizontal`);
  the event `ScrollEnded` (final, `Scroll $scroll`, `int $x`, `int $y`): once when the content comes to rest after a
  release or at the end of a `scrollTo()` (after its `TweenDone`), none for a scroll freed before its motion ends (one
  freed after its end was queued gets an event with a stand-in `Scroll`). `\ValueError` `Baremetal\Slate\Scroll::$w must
  be between 1 and 32767` (`$h`), `...::$clip is always true`, `...::$snapX must be between 0 and 32767` (`$snapY`,
  `$snapMax`, `$contentW`, `$contentH`), `...::$scrollX must be a finite number` (`$scrollY`, and `scrollTo()`'s `$x`,
  `$y`), `...::$scrollX cannot be animated; use scrollTo()` (`animate()`/`ease()` of `$scrollX`, `$scrollY`,
  `$contentW`, `$contentH`, `$snapX`, `$snapY`, `$snapMax`), `ms must be 0 or more`. A scene without a moving scroll
  does no more work per frame, and a still screen composes no frame. Engine side: the command `SLATE_CMD_SCROLL`
  (`slate_scroll_cmd_t`), the position published without a lock (`slate_scroll_pub_t`), `slate_engine_scroll_done`, the
  stats `scrolls_active`, `anim_ticks` and the compositor's `scroll_nocache`; the physics in `src/scroll.h` (pure C).
  Docs: `docs/usage/scroll.md`; IDE stub.
- Added: the engine side of tweens (`src/tween.h`: easing curves, sampling from the elapsed time, delay, repeat,
  yoyo, pause, chains; `src/engine.h`, Tweens: the commands `SLATE_CMD_TWEEN`, `SLATE_CMD_TWEEN_STOP`,
  `SLATE_CMD_EASE`, the tick at the vsync time of the frame, a frame per pulse only while tweens run, tweens of a
  scene not shown paused, the done queue `slate_engine_tween_done`, drained by the PHP layer from the tween listener
  on the engine's task: one ext-events event per id). A scene without tweens does no more work per frame.
  Slate takes one of the four idle hooks of ext-events, shared by the running Slate, the images, the animations and
  the transitions.
- Added: page transitions. `Slate::show(Scene $s, ?Transition $t = null)`: with `$t` the new scene comes in while
  the compositor composes both scenes together. `final class Transition` (private constructor, no clone):
  `Transition::fade(int $ms = 300, Ease $ease = Ease::OutCubic)`, `Transition::push(Direction $d, int $ms = 300,
  Ease $ease = Ease::OutCubic)`, `Transition::cover(...)` and `Transition::uncover(...)` with the same parameters as
  `push()` (`\ValueError` `ms must be at least 1`, `ms must be at most 2147483647`); the enum `Direction` (`Left`,
  `Right`, `Up`, `Down`: the direction of the movement); the event `TransitionDone` (final, `Scene $from`,
  `Scene $to`). Order: `build()` the first time and `onEnter()` of the new scene at once, `onLeave()` of the old one
  and `TransitionDone` at the end (`onLeave()` runs from Slate's own watch of the event, before the listeners);
  `scene()` is the new scene meanwhile; the touch reaches no scene from `show()` to the end. `show()` of the scene
  shown (also of the one a transition goes to) does nothing; `show()` during a transition ends it first
  (`onLeave()` of the scene it leaves at once, its `TransitionDone` follows), then the new change starts. A
  transition from no scene (the first `show()`) comes in over the background, with no `TransitionDone`. When its
  event is not delivered (`init-loop`, a full pool) the `onLeave()` runs at the next `show()` or `commit()`. When
  the Slate stops during a transition neither comes. A transition that cannot start (no memory for its layers) is a
  cut, still with its `onLeave()` and `TransitionDone`. `\RuntimeException` `Slate: cannot run a transition: the
  table of ext-events' idle hooks is full` (or `... watches is full`: Slate takes one of the eight watches of
  ext-events for TransitionDone, besides the four of the touch) and `Slate: out of memory for the transition`.
  Without `$t`, `show()` is instant as before. Docs:
  `docs/usage/scenes.md` (Transitions), `docs/usage/animations.md`; IDE stub.
- Added: the engine side of transitions (`src/compose.h`, Transitions: `slate_pair_begin` / `slate_pair_at` /
  `slate_pair_end`, two scene roots in layers under the screen root, each cut to its page, the top one over a curtain
  of its background, moved by whole pixels or composed offscreen with opacity in strips of the existing scratch: no
  new screen-sized buffer, both bands as before; caches built for their page and moved with it; the strips of a
  group sized for what its nested groups need; `slate_compose_underlay_free()`). `src/cmd.h`: a SHOW carries a
  `slate_trans_t` (kind, direction, ease, ms) next to the PIXELS rectangle; `slate_trans_ok()`; a bad transition
  makes the push return `SLATE_CMD_BAD_ARG`. `src/engine.h`, Transitions: the start at the batch's time, the ticks
  with the tweens' (the whole screen dirty only when the layers moved), the end at its time also while the panel
  sleeps, a SHOW during a transition ending it first (a SHOW of the scene it goes to only changes the name and the
  background), REMOVE of either scene refused and FREE ending it, the underlay off meanwhile; the done queue
  `slate_engine_transition_done()` (the roots' ids, 64 waiting), drained from the tween listener; a transition
  that cannot begin (no memory for its layers) is a cut that still queues its done;
  `slate_engine_in_transition()`; `stats.transitions` and `stats.transition_dones_lost`. A scene without a
  transition does no more work per frame.
- Changed: a scene left keeps its caches (cached groups, shadows, blurs) for a return to it while there is room:
  showing it again (also with a transition) builds nothing. `Slate::$keepScenes` (`int`, default 2; 0: every scene
  left drops its caches, as in 1.1), `Slate::$keptBudget` (`?int` KB, default null: only the cache budget; 0:
  nothing kept), `Scene::$keepCache` (protected `bool`, default true; false: dropped when the scene is left, read at
  the `show()` that leaves it). Past `$keepScenes` or `$keptBudget` the scene left first drops its caches at once,
  also when they are lowered. The kept caches stay in the existing cache budget in PSRAM (no internal RAM, no new
  pool): when the scene shown or the underlay needs room there, the scenes left drop theirs first, the one left
  first before the others. Images are outside that budget, but an image that cannot be loaded for lack of memory
  has the scenes left drop their caches and is loaded once more (only then `no-memory`; `docs/usage/images.md`). `\ValueError` `keepScenes must be 0
  or more`, `keptBudget must be 0 or more` (the old value stays); neither can be taken by reference or unset. IDE
  stub and `docs/usage/scenes.md` (Returning to a scene), `docs/usage/performance.md`.
- Added: the engine side of the kept scenes (`src/engine.h`, Kept scenes): `slate_engine_set_keep(e, scenes,
  budget_bytes)` (applied at the next wake-up; engine default 0, as before), the SHOW's `keep` flag (`src/cmd.h`: the
  scene it leaves may keep its caches), the list of kept roots, oldest first; the engine's `cache_alloc` takes their
  room first, oldest first, only when that makes the block fit (in the budget or in external RAM); a SHOW of a kept
  root or an ADD of it into a group takes it off the list, FREE of a kept root (or of an element in one) and REMOVE
  of an element from one drop the caches under it first, an ADD into a kept scene counts the caches it brings; a
  SHOW applied after `slate_engine_set_keep` always uses the new limits; the image loader's reclaim
  (`slate_images_set_reclaim()` in `src/images.h`, set by the engine at start: out of memory for an image, the
  loader sets a flag and kicks the engine, the engine drops every kept cache on its own task and answers on a
  queue, the loader waits at most `SLATE_RECLAIM_WAIT_MS` (100 ms) and reads the file once more when something
  was freed or no answer came; only a block the store's allocator refused asks; without an engine, with one
  stopping, or with nothing kept, it fails at once as before; `slate_engine_stop` cuts the loader off first and
  waits, by the clock, for a request under way before it frees anything); `stats.kept_scenes`,
  `stats.kept_bytes`, `stats.kept_dropped`, `stats.reclaims`. No work per frame.
- Fixed: waits that loop until another task is done now block at least `SLATE_NAP_MS` (10 ms, one tick at
  `CONFIG_FREERTOS_HZ = 100`) per turn: with a 100 Hz tick a wait of 1 ms is 0 ticks, a busy loop that could starve
  a lower-priority task on the same core (the esp32 SDK runs a 1 kHz tick, where 1 ms blocks). `sl_images_engine(NULL)`
  (the stop of a Slate waiting for an image listener's kick under way) waited this way.
- Fixed: the default cache budget is half of the largest external block within 1/16, no longer half of the power of
  two below it. With 1 008 KB free in one block at start it was 256 KB, short of the underlay of a 360 x 360
  screen (about 260 KB): the busy scene of `slate-stress` lost its underlay and ran at 35 frames per second
  instead of 55. It is now 496 KB. On boards with about 1.1 to 2 MB free PSRAM in one block the default budget rises
  too (1.9 MB gives about 930 KB, it was 512 KB): more room for the live and kept caches, less headroom for images.
- Fixed: when the stop of a Slate fails because an image load waiting for memory did not end in time, the warning
  says so (`Slate: the compositor did not stop cleanly (an image load waiting for memory did not end in time); its
  memory stays`) instead of blaming the panel.
- Changed: the compositor's task has a 10 KB stack (`SLATE_ENGINE_STACK`, it was 8 KB): a cached group with a shadow
  built inside the offscreen layer of a fade used 6.3 KB, and deeper nesting of cached and blurred groups needs
  more. About 2 KB more of internal RAM.
- Fixed: a tween and a page transition of the same length started in one batch now start on the same time (the
  batch's, the panel pulse it came with) and end on the same frame; the tween started a little later before.
- Fixed: writing to an element of a scene that is kept but not shown no longer leaves the kept-cache count high,
  which made the cache budget evict the kept scenes for a block that did not fit anyway.

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
  raised again). Docs: `docs/usage/images.md` (assets, board paths `/app/...` and `/sdcard/...`, memory of images, which comes from the PSRAM the SDK leaves outside PHP: the esp32 SDK's `firmware-reserve`, and memory of on-board PNG),
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
