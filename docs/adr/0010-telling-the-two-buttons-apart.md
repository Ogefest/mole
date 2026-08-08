# ADR-0010: The two buttons in a dialog are told apart

- **Date:** 2026-08-09
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
