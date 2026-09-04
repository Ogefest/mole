# ADR-0072: Colour lives in one place and a view names a token

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

`src/app/ui` held 372 colour literals across 39 QML files, 75 distinct values
between them, and nothing that said what the window was painted in. Eight views
declared their own `panelColor`, `lineColor`, `mutedColor`, `warnColor`,
`badColor` and `goodColor` rather than reading one, and
`Material.background: "#1b2029"` was written out in ten of them.

They had already drifted, which is what made it a fault rather than untidiness.
`AutomationView.qml` used `#1b1d21`, `#2c2f36` and `#8b919b` where the rest of the
window used `#1b2029`, `#2a3140` and `#8b93a7`; `TerminalPanel.qml` had a third
panel colour again, `#12151b`. Nobody chose three families of grey. They arrived
one view at a time, and the next view would have added a fourth. Near-white had
drifted the same way — `#e6ebf5`, `#d5dbe6`, `#d7dae0`, `#c9d1e0`, `#c9d1d9`,
`#c8cedb`, `#c8cede` — as had every grey between a hairline and disabled text.

Changing what Mole looks like therefore meant editing 39 files, and Mole is about
to ship four themes with the choice in the `View` menu (MOLE-279, MOLE-280).

The type scale had already solved this once. `AppController` picks the sizes and
the monospace family in one place, in its own words "so that a listing, a preview
and a form line up instead of each picking its own number", and every view reads
`App.textSize`.

## Decision

**Colour is sixteen named tokens on one object, and a view names a token rather
than a value.**

`mole::Palette` (`src/ui/Palette.h`) is a `QObject` carrying one `Q_PROPERTY` per
token, exposed as `Q_PROPERTY(mole::Palette* colour READ colour CONSTANT)` on
`AppController` and read from QML as `App.colour.panel`. The sixteen are
`window`, `panel`, `pane`, `border`, `hover`, `selection`, `text`,
`textSecondary`, `textMuted`, `textFaint`, `accent`, `link`, `ok`, `warn`, `bad`
and `busy`. Each has a job, and the job is written next to it in the header,
because from here on every view names one of them.

**The pointer is `CONSTANT`; the tokens inside it are not.** The palette itself
never changes, so that property says so. Every token property notifies, through
one signal shared by all sixteen.

**A view may derive from a token, and only in two shapes.** `Qt.alpha(token, a)`
where the role is that same colour, weaker — a banner's ground under its own
coloured hairline, a badge's border under its own coloured label. And a colour
that is not the window's paint at all: `"white"` for a sheet of paper inside a
document preview, `Qt.rgba(0, 0, 0, 0.32)` for a veil that darkens whatever is
behind it. Nothing else. `tst_QmlConventions` holds the rule by reading the
source, so it goes on being true for files nobody has written yet.

**A document's colours are not the window's paint.** The syntax highlighter's
token colours, `MarkdownStyle.h` and the terminal's sixteen ANSI colours stay
out of the token layer: a theme has no business deciding what a keyword looks
like. The ANSI table moved from `TerminalPanel.qml` to
`TerminalController::ansiPalette()` — the same values, next to the parser that
produces the indices — so that the panel's *chrome* can adopt `panel` while its
*content* keeps its own vocabulary. The panel a terminal sits in is chrome.

## Reason

**Sixteen tokens rather than a value per site.** The alternative was to preserve
all 75 values, which needs 75 tokens, which is not a palette — it is the same
scattering with one more level of indirection, and a theme would have to state 75
values instead of sixteen. Collapsing is the work: five of those 75 values were
the *same* grey four times over.

**A nested object rather than flat properties on `AppController`.** Flat would
work and would add twenty-odd lines to a header that already carries thirty.
`tabs`, `drives` and `tasks` are already nested objects; this follows them.

**`QColor` properties rather than a lookup method.** A method call in a QML
binding is evaluated once and never again — there is no signal to tell the
binding it went stale — which is the same trap `driveKinds` and `screenRows`
already exist to avoid.

**Costs paid, and stated here because a reader will otherwise think they were
missed.** The refactor was meant to prove itself by leaving the user guide's
pictures alone. It does not: all 54 move. Three visible corrections cause it, and
the first of the three is in every picture.

- The sidebar's capacity bar was filled with `#4f8cc9`, a steel blue one step off
  the accent that nobody had named. It becomes `accent`. The bar is four pixels
  tall and appears in all 54 pictures, so all 54 changed; `compare-shots` reports
  it as roughly 1,600 pixels, which is exactly the two bars.
- `AutomationView.qml` and `TerminalPanel.qml` adopt the window's family, which
  is the drift above being corrected on purpose.
- Where two families of near-white or two greys between `border` and `textFaint`
  met on the same screen, they became one token. A hex dump's bytes and its ASCII
  column are the largest instance.

Keeping the guide still would have meant keeping the drift, and the drift is the
fault this record exists to close.

## Consequences

- A theme is now sixteen values, and no view changes when one is chosen. That is
  what MOLE-279 builds on.
- `Material.primary`, `Material.accent` and `Material.background` in `Main.qml`
  read from tokens, so the ten copies of `Material.background: "#1b2029"` are one
  token. `Material.theme` stays `Material.Dark`: it is a polarity rather than a
  colour, and it becomes a question the moment there is a light palette to answer
  it with (MOLE-280).
- A new view that wants a colour that is not one of the sixteen has to argue for a
  seventeenth token rather than write a value down, and `tst_QmlConventions` is
  where the argument gets stopped until it is had.
- The drag badge in `main.cpp` is painted with `QPainter` rather than by the scene
  graph, and reads the same tokens. Anything else outside `src/app/ui` that paints
  part of this window should do the same.

## Amendment, 2026-09-04 (MOLE-397)

**"A view names a token and may derive from one in two shapes. Nothing else" was
enforced by a grep for `#rrggbb`**, so every other way round the token layer
passed the check — and five of them were in the tree.

- `Material.foreground` painted **the most-read text in the application**: the
  file name in both `FilePane.qml` delegates and the sidebar's labels. It is the
  Material style's computed text colour for the polarity, not `App.colour.text`,
  so a theme that set `text` to anything but Material's default had no effect on
  the listing at all. ADR-0073 says "no view changes when one is chosen"; this was
  a view that did not change when it should have. `tst_Palette`'s contrast table
  asserted `text` against `pane` — a pair the listing never painted.
- `Material.accent` was a colour *value* in twenty-four places across twelve
  files, tracking the theme only because `Main.qml` binds it — and those same
  files named `App.colour.accent` elsewhere, so one file named one token two ways.
- Derivations in four shapes the ADR does not allow:
  `Qt.lighter(App.colour.window, 1.1)` for the two split handles,
  `Qt.darker(App.colour.accent, 1.3)` in `ActionButton.qml` and
  `ConfirmButtons.qml`, and `Qt.hsla((index * 0.13) % 1.0, 0.45, 0.58, 1.0)` for
  the analysis chart — a generated categorical palette with one fixed lightness
  for every theme there is, never looked at on a light ground.
- An opacity standing in for a token: the accent at 0.18 where `selection`
  exists, a pane at 0.85 for an inactive frame, two hex marks at 0.28, and
  disabled words at 0.4.

**Derivations are allowed, and they live in the palette.** `divider`,
`accentPressed`, `mark` and `categorical` are computed from the sixteen, once,
with the polarity in hand — so a theme still repaints everything at once and a
view still names a token. The two shapes the original decision allowed in a view
are now none: a view that wants a derived colour asks for a derived token, the
same way it would argue for a seventeenth.

What that bought, measured rather than asserted: `divider` is the hairline colour,
because a three-quarter step from the window towards it left 1.12:1 on the light
theme — under the floor a hairline itself is held to. `accentPressed` moves *away*
from the ground rather than always darker, which is what `Qt.darker` got wrong on
a dark theme. `categorical` is five tokens and not six: `busy` is a ground and at
1.13:1 against the panel it would have been a bar nobody could see.

`tst_QmlConventions::nothingPaintsWithTheStyleOrADerivation` refuses all of it now
— reading `Material.accent`/`foreground`/`primary`, `Qt.hsla`, `Qt.lighter`,
`Qt.darker`, and an opacity on something that paints a colour. Setting the style's
own properties from tokens is untouched, which is what `Main.qml` does and what
makes Qt's own controls follow the theme. Two allowances are named in the test:
`Qt.rgba` for a veil over whatever is behind it (`DimVeil.qml`), and a
translucency over the terminal's own sixteen colours, where seeing the character
under the cursor is the requirement rather than a colour nobody chose.
