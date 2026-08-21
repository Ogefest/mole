# ADR-0074: A theme has a polarity, and what follows it

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

[ADR-0072](0072-colour-lives-in-one-place-and-a-view-names-a-token.md) put colour
into sixteen tokens and
[ADR-0073](0073-a-theme-is-sixteen-values-and-the-choice-is-a-preference.md) made
a theme a choice. Both dark themes cost nothing beyond their sixteen values,
because nothing in the tree had to be reconsidered.

`Paper` and `Workbench` cannot join the chooser the same way. **A light window is
not a palette swap.** Several things never read the palette at all and were
written against a dark ground, and each of them comes out dark-slab-on-white
wrong. Four were known when the work started and five more were found by looking
at a light window rather than by reasoning about one:

- `Material.theme: Material.Dark`, one line, deciding everything a Material
  control draws for itself — a text field's underline and placeholder, a combo
  box's popup, a dialog's ground, a ripple, a disabled label's opacity.
- The source highlighter's nine colours. `kStringColour` was `#a5d6a7`, a pastel
  green picked against `#151922`; on white it is about 1.7:1 and a string literal
  stops being text.
- `MarkdownStyle`'s three: a `#232a36` slab behind a code fence is a dark
  rectangle in the middle of a white page.
- The terminal's sixteen ANSI colours on a dark ground.

And, found by looking:

- **`Material.background` set on the window.** It propagates to every control,
  and Material asks the *inherited* background for a highlighted button's fill
  while painting that button's label white regardless of theme. So the primary
  action in six views — Search, Scan, Preview, Apply, Save, Scan now — was white
  on white under a light palette. Under a dark one the same accident made the fill
  match the toolbar behind it, which is why nobody had noticed that the active
  layout in the browser's mode switch was marked by a drop shadow and nothing
  else, in flat contradiction of the comment above it.
- **`Material.color(Material.Red / Amber / Green)`**, in twenty places. Material's
  fixed palette has no polarity: Amber on white is 1.7:1, Green 2.8:1, Red 3.4:1.
- **`ConfirmButtons`' acting button**, whose label was `text` on an `accent` fill.
  ADR-0072 collapsed `#2d6cdf` into `accent`, and `accent` is a *light* blue on the
  dark themes — near-white on it is 2.2:1. A fault this record's own test found.
- **Two `SplitView` handles**, painted by the style from `Material.background`.
- **The drag badge in `main.cpp`**, which copied the tokens once at startup.

## Decision

**A theme states its polarity; nothing derives one.** `Palette::light` is a
property, and `Palette::isLightTheme(name)` a static. Sixteen colours cannot be
interrogated for a polarity reliably, and guessing from the luminance of `window`
is the kind of cleverness that holds for the four themes it was written against
and fails on the fifth.

**`Material.theme` binds to it**, and the window's own ground moves from
`Material.background` to `Window.color`. A control that wants the palette's ground
asks for it by name — every dialog and popup now says
`Material.background: App.colour.panel`, which is what the token table always
meant by "dialog grounds".

**A document's colours follow the polarity, not the palette.** ADR-0072's
reasoning survives and its conclusion does not: a theme still has no business
deciding what a keyword looks like, so the highlighter's nine and Markdown's slab
are not tokens — but they cannot ignore the window either. So there are two sets
and the polarity says which is in force, and a document already open is
**re-coloured** rather than left as it was until the next file is opened. The view
passes the polarity down, because the view is what knows the palette.

Markdown's three are the exception inside the exception: a code slab, a quotation
and a table rule are *the window's paint appearing inside a page*, so they are
tokens after all — `hover`, `textMuted` and `border`, passed by the view.

**The terminal stays dark on both polarities.** Its chrome — the panel, the
toolbar, the title — follows the palette; its screen keeps colour zero as its
ground and colour seven as its foreground, and everything drawn on that screen,
including the cursor block and the "no pseudo-terminal" message, comes from the
same sixteen. A terminal is dark inside a light application everywhere else, a
shell that prints a colour has no way of knowing what it will land on, and the
sixteen are the terminal's data rather than the window's paint.

**A filled control's label is `window`.** The accent is a light blue on the dark
themes and a dark one on the light themes, so the label has to come from the far
side of the polarity either way, and `window` is that token by construction.
`ActionButton.qml` is the one place it is written; `ConfirmButtons` follows the
same rule.

**`DimVeil` needs nothing, and it is worth writing down so it is not reopened.**
Black at thirty-two percent is what Material's specification asks for on both
polarities: a light window dimmed by darkening is correct. The walkthrough's pixel
assertion about it is taken on `Midnight` and stays valid.

**A contrast floor is asserted per pair, over every theme.** `tst_Palette` holds a
table of the pairs that actually meet on screen with a floor and a reason for each
— 4.5:1 for text at body size, 3.0:1 for a focus ring or a four-pixel bar, 4.0:1
for a semantic colour that carries a word but never carries it alone (ADR-0010),
2.5:1 for the placeholder floor that WCAG exempts and that only has to remain
visible, 1.15:1 for a hairline. The highlighter's nine are in it at 4.5:1, except
the comment at 3.0:1, because a comment is the thing in a source file you are
meant to be able to skip. Every theme added after these four meets the same
numbers.

## Reason

**Looking beat reasoning, and that is the finding worth keeping.** Four faults
were predicted; five more were found by rendering the whole guide under `Paper`
and reading it. The white-on-white primary button is the one that matters: it was
invisible to every kind of review, because on the dark theme it looked deliberate.

**Four of the ticket's stated values moved, and the contrast test is why.**
`Slate`'s `textMuted`, `Paper`'s `textMuted`, and `Workbench`'s `textMuted` and
`textFaint` were each a few percent under their floor — `Workbench`'s `textMuted`
on `window` was 4.38:1 against a floor of 4.5. They were nudged until they passed.
`Midnight` was not touched and passes every floor as it stands, which is the
answer to the obvious worry about a test written after the values.

**The mode switch was redrawn rather than patched.** Material paints a highlighted
label white whatever the theme, so no fill from the palette can be relied on to
sit under it. The current mode is now a filled pill *and* bold — two channels,
because ADR-0010 says colour must not be the only one, and because the previous
single channel was a drop shadow that nobody could see.

**The alternative for the highlighter was to make its nine colours tokens.**
Rejected: two themes of the same polarity should colour the same source file the
same way, and a `Slate` that recoloured Python would be a theme deciding something
that is not its business.

## Consequences

- Four themes, two of each polarity, and the chooser needed nothing new.
- Thirty-five of the guide's fifty-four pictures are rewritten. The mode switch
  is in nearly all of them and the primary buttons in six; both are visible
  improvements on `Midnight` as well, and neither was avoidable while keeping the
  light themes legible.
- `src/app/ui` now holds no colour that is not a token or a documented derivation
  of one: the last twenty `Material.color()` calls became `ok`, `warn` and `bad`.
- A theme change repaints a running window *and* re-colours an open document. The
  test for the second is in `tst_Preview`, because that is the one that will be
  missed: a document is formatted when it is loaded and nothing asks again.
- The `Picker` dropdown, and every other popup, now names its own ground. Anything
  new that opens over the window has to as well; the window no longer hands one
  down, and Material's own grey is a colour nobody in this application chose.
