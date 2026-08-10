# ADR-0031: A drive that needs the credential store is connected when it is opened

- **Date:** 2026-08-10
- **Status:** Accepted

## Context

Passwords for network drives live in an encrypted store, opened once with a
passphrase. The store is shut at every startup, and it may stay shut for a whole
session — most sessions never touch a drive that needs it.

The arrangement before this asked at startup. A band above the drive list appeared
whenever anything was waiting, carrying the explanation, the field and the button;
every drive whose password was in the store wore an amber dot, the same colour a
drive on its way to failing wears. All of it before anybody had asked for any of
those drives. That was a deliberate answer to a worse problem — a drive whose
password was in a shut store used to wait at startup in *complete silence*, with
no prompt, no badge and no failure, and the only way to find out why was to open a
dialog and notice an amber panel. It was recorded in the commit that closed
MOLE-44 rather than in a record of its own, which is part of why the trade in it
went unexamined for so long.

Two things were wrong with it.

**The question was asked at the one moment nobody has a reason to answer.** Nothing
has gone wrong at startup; the store is simply shut, which is its normal state.
Asking then trains people to dismiss the question, and dismissing it is the same
gesture as ignoring a real warning.

**The sidebar is the wrong shape to type a password into.** A label, a field and a
button share about two hundred pixels of a panel that is 240 wide and can be dragged
down to 160.

And separately: opening an unconnected drive did nothing useful. `goTo()` handed the
uri straight to the pane, `VfsManager::resolve()` found no mount, and the drive was
shown as a folder with nothing in it — which reads as an empty drive rather than as
one that is not connected.

## Decision

**A drive is connected by being opened.** `AppController::goTo()` — where the sidebar
row, the command palette and the bookmarks all arrive — connects the configured drive
behind a uri when nothing is mounted there. A connect that fails says so, rather than
leaving an empty listing to be misread.

**When that drive needs the credential store and the store is shut, the passphrase is
asked for at that moment, and the navigation follows once it is open.** Not asked for
again later, and not owed a session later either: backing out of the question drops
the navigation with it.

**The question is a modal dialog, centred on the window** — `ui/UnlockDialog.qml`,
with the shared footer and the verb *Unlock*, filled and not red, since opening a
store destroys nothing. It is the only place the copy about the passphrase lives; the
drives dialog opens the same dialog rather than growing a second copy of the sentence.

**Nothing asks at startup.** No band, no dialog. A drive marked *connect at startup*
with no secrets is still connected with nothing typed, and typing the passphrase once
still brings up everything that was waiting on it — that is what the setting means.

**A drive waiting on the store reads `idle`, not `attention`.** Grey, with the local
disks and the drives nobody has connected. It still *says* `Locked`, its tooltip still
says so, and its row still offers a key rather than a play triangle: the words keep the
distinction the colour gives up.

## Reason

This **reverses half of MOLE-44's decision** — that the store must never be asked for
in a modal — and keeps the reason behind it, which is why only half is reversed. A
modal at startup, for a drive nobody has asked for, is still the wrong trade. This one
only ever appears because somebody just asked for the drive it belongs to, and at that
moment a modal is not an interruption: it is the next step of what they were doing.

The alternatives considered:

- **Leave the band and add the connect-on-open.** Rejected: the band would then ask at
  startup for something the flow no longer needs asked, and two ways to open the store
  is two things to keep working.
- **Ask in the pane, where the empty listing is.** Rejected: the pane is where the
  answer goes, not where the question belongs, and every tab would need its own copy.
- **Connect on open but fail with a message when the store is shut.** Rejected: it turns
  one gesture into two, and the second one is "go and find where the passphrase is
  typed" — which is the problem MOLE-44 was about.

Keeping `Locked` in the words while dropping it from the colour is the part most likely
to be questioned later. Amber is for something that needs attention now; a drive nobody
has opened does not. Losing the amber costs a reader nothing they cannot get from the
row itself, and it buys back the meaning of amber everywhere else in the list.

## Consequences

- `goTo()` returns whether it navigated. A caller that hands the keyboard to wherever
  it just went — the sidebar row does — must not hand it to a place nobody arrived at:
  taking the keyboard out of the modal that has just appeared leaves that modal holding
  no focus at all, and then nothing inside it can take the keyboard either.
- `RemoteRegistry::driveForUri()` exists so the rule that a drive's scheme comes from
  its name stays in the registry rather than being re-derived by a caller.
- `ui/UnlockBand.qml` is gone. Its five tests are rewritten against the dialog and the
  new flow rather than deleted: they were the tests for the old flow, not tests that
  happened to touch it.
- The guide's *Drives* page describes the new flow, and `images/11d-drive-locked.png`
  shows a locked row rather than a band.
- A pending navigation is held in the controller for as long as the dialog is up. It is
  one uri and it is dropped when the dialog is refused, so nothing accumulates.
