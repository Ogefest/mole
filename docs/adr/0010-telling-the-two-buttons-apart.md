# ADR-0010: The two buttons in a dialog are told apart

- **Date:** 2026-08-09
- **Amended:** 2026-08-10 — the reach, not the decision. See *Amendment* at the end.
- **Status:** Accepted

## Context

Reported plainly: on the popups it is not clear which button is the active one —
they look the same, and it is easy to hit the wrong one.

Measured, that was worse than it sounds. The standard buttons rendered as two flat
labels of the same size, weight and colour; **neither of them held the keyboard**,
so there was no focus to see either. Over the delete dialog the pair read *Yes* and
*No*, which says nothing about which one deletes.

## Decision

**One shared footer, `ConfirmButtons.qml`, on every dialog with two outcomes.**

**The button that acts is filled; the one that backs out is outlined.** Fill is the
strongest signal available at a glance and costs nothing to read.

**The acting button is red when what it does cannot be undone**, blue otherwise.

**Buttons are labelled with the verb**: *Delete*, *Compress*, *Rename*, *Create* —
not *Ok*. A verb tells you what the button does without reading the dialog again;
*Ok* and *Cancel* only tell you which one is on the right.

**A destructive dialog opens with the keyboard on the way out**, and the focused
button is outlined so that is visible. A stray Return or Space closes the question
rather than answering it irreversibly.

**Dialogs that ask for text keep the keyboard in the field.** Compressing, renaming
and creating a folder are typed into first, so the field wins over the button.

**The backgrounds are drawn here rather than left to the style.** This is the part
worth recording, because it cost the most time: asking the Material style for a
`highlighted` button gives an item that reports itself visible, correctly sized and
filled with the colour asked for — and paints nothing. Every property said the button
was red. A screenshot with no red pixel anywhere in the window is what settled it.

## Reason

Colour alone was rejected as the only signal: it fails for anybody who cannot separate
red from grey, which is why the fill, the verb and the focus outline all carry the
same message independently.

Making Return mean *No* in a destructive dialog is a deliberate behaviour change. The
alternative — Return means the destructive answer — is the arrangement that turns a
half-read dialog into deleted data. Nothing else here answers a question by pressing
Return blindly, so nothing is lost by it.

## Consequences

- The button state is tested by reading the colours off the items rather than the
  style properties, since the properties were the thing that lied. The delete
  confirmation asserts the acting button is red, the way out is unfilled, and the
  keyboard is on the way out.
- A new dialog gets this by using the shared footer; one that sets `standardButtons`
  instead goes back to two identical labels, so the footer is the thing to reach for.
- Dialogs with a single *Close* button are left alone: there is nothing to tell apart.

## Amendment, 2026-08-10 (MOLE-100)

The decision above is unchanged. Three things were wrong about its reach, and the
first of them is why the other two are worth writing down.

**It was opt-in, and opt-in did not hold.** This ADR converted six dialogs of the
thirteen in the window and predicted, in the consequence above, that a dialog
setting `standardButtons` would go back to two identical labels. Three did — the copy
and move dialog, the index-a-folder dialog, and the one that deletes every saved
report for a folder, which offered that on a button labelled *Ok* in the same grey as
the one beside it, with the keyboard on neither. Nothing failed when a dialog did not
reach for the footer, and nobody was watching.

**So the window has no use for `standardButtons` at all, and the build says so.**
A configure-time check in `src/app/CMakeLists.txt` fails, naming the file, when any
QML file under `src/app/ui` sets it — the same shape of check as the one beside it that
catches a QML file missing from the application (MOLE-79). A grep, not a parser. A rule
enforced by remembering is a rule that lasts until the next contributor.

**A single-button dialog holds the keyboard on its one button.** Leaving those alone
was right about the appearance and wrong about the keyboard: with nothing focused there
is no focus ring and Return does nothing, so the only ways out of a window that exists
to be read were Escape and the mouse. They use the same footer with `dismissOnly: true`
— one outlined button, labelled *Close*, holding the keyboard.

**Where the keyboard starts is now stated rather than implied.** `keyboardOn` takes
`"accept"`, `"reject"` or `"none"`, defaulting to the way out for a destructive dialog
and to the acting button otherwise, so an ordinary question can be answered with Return.
`"none"` is for a dialog that puts the keyboard in a field of its own — this ADR's
existing rule — and it is the dialog's job to focus that field. Never leave it nowhere.

Two things learned building it, both worth the paragraph:

- **`focus: true` on the right button is not enough.** `DialogButtonBox` lays its
  buttons out with a `ListView`, and a `ListView` hands the keyboard to its own current
  item — whichever button happens to be first. The declarative answer was overruled in
  silence, so the footer places the keyboard imperatively as well.
- **A focused `Button` answers Space and not Return.** Return is a dialog-level key,
  and Qt Quick's `Dialog` only turns it into an answer when a standard button is in
  charge — which is the thing this footer replaced. The footer handles Return itself, or
  the focus ring sits on a button that Return does not press.
