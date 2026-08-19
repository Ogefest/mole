# ADR-0053: A video preview plays itself

- **Date:** 2026-08-19
- **Status:** Accepted
- **Revised:** 2026-08-19 — the contingency below fired the same day: the viewer has a
  mute control, and it is remembered. See *Revision* at the end.

## Context

MOLE-37 gave video a viewer and deliberately left it stopped: `F3` opened a file
paused at its first frame, with a play button under it. The reason was written down
in three places, and it is a good one — **`F3` walks a folder with `←`/`→`**, one
preview tab reused for every file, so a viewer that starts on its own is a viewer
that starts making noise as the cursor passes over a file. Nobody wants a folder of
holiday clips shouting at them on the way past.

Used for an afternoon, the cost of that turned out to fall on the common case rather
than the rare one. Pressing `F3` on a video is not walking past it — it is asking
what is in the file, and for a video the answer is the first few seconds of it, not a
still. The still frame is the one thing a person already had: the listing shows the
name and the details panel shows the codecs. So every deliberate look at a video cost
a second action that carried no decision, and the argument protecting that cost was
about a different action.

## Decision

**A video preview starts playing as soon as the file is loaded**, and that includes
one arrived at by stepping with the arrows. `MediaPlayer` is told to `play()` on the
`LoadedMedia` transition, where it was told to `pause()`.

Nothing else about the viewer changes: no volume slider, no playlist, no frame
stepping, and the button under the video still says what it will do — it now reads
*Pause* on arrival rather than *Play*.

## Reason

**One rule that is always true beats a rule with a good exception.** The alternative
that keeps MOLE-37's argument intact is to play on `F3` and stay paused when the file
is stepped onto, and the tab controller can tell those apart — `open()` and `step()`
are different entry points. It was rejected on what it produces rather than on what
it costs to build: the same file plays or does not depending on how somebody reached
it, which is a rule that has to be learnt and then remembered, and the surprise lands
on whoever is walking a folder deliberately looking for the right clip.

**Muted autoplay was rejected as half a preview.** It answers the noise and keeps one
rule, and a video preview with no sound cannot answer *is this the take with the
audio in it*. Worse, this viewer has no volume control by design, so a muted default
would be a decision the reader cannot undo — the one shape of default worth avoiding
above all others.

**The accepted cost is stated rather than discovered.** Walking a folder of videos
with the arrows now plays each of them, sound included. What makes that survivable is
that stepping is a key press per file and a person doing it has their hand on the
keyboard: the file changes and the previous player is destroyed with its viewer, so
what is heard is one clip, not a pile of them. If this becomes the louder complaint,
the fix is not to go back to a paused first frame — it is to give the viewer the
volume control it does not have, and to remember the setting the way ADR-0006
remembers a viewer's other choices.

## Consequences

- `docs/guide/previews.md` teaches the new behaviour, and stops explaining at length
  why a video does not start on its own.
- The walkthrough test asserts *playing without being asked* and pausing on the
  button. It asserts the transition — that the clip moved on its own — rather than
  `PlayingState`, which stops being true the moment a short clip reaches its end. The
  fixture is five seconds rather than one for the same reason.
- A viewer left running behind the next preview would now be a live fault rather than
  a theoretical one, since every video preview is running. What prevents it is
  unchanged and was already belt and braces: the tab destroys the viewer when the
  file changes, and `Component.onDestruction` stops the player.
- MOLE-37's reasoning is not deleted anywhere. The code comments say what the
  argument was and that it was overruled, because a reader who meets autoplay and
  thinks *surely somebody considered the noise* deserves to find that somebody did.

## Revision, 2026-08-19: the contingency fired

The paragraph above said that if the noise became the louder complaint, the answer was
not a paused first frame again but the volume control this viewer does not have,
remembered the way ADR-0006 remembers a viewer's other choices. It became the louder
complaint within the hour, which is a fair result rather than an embarrassing one: the
guess about *which* cost would be felt was wrong, and the guess about *what to do
about it* was written down and held.

**The viewer has a mute button, and whether the sound is off is remembered for every
video and across restarts.** A speaker glyph in the controls, `App.minimumTarget`
square like every other icon-only control in this window, and an `AudioOutput` whose
`muted` is bound to the controller rather than held in the view.

**Mute rather than a volume slider.** A slider is a value to argue about, to draw, and
to remember to a precision nobody asked for, in a viewer whose stated point is that it
is small. Mute is the question actually being asked — *not in this room, not right
now* — and it is answered in one press and undone in one press. If somebody one day
wants a video preview at 30% volume, that is a different request and gets its own
decision.

**One key for every video, not one per suffix, and that is the opposite of ADR-0006.**
`preview.video.muted`. ADR-0006 keys a viewer *option* by provider and suffix because
*render this `.html` as a page* is a choice about a file type, and choosing it for
`.html` must not answer for `.xml`. Whether the room is quiet is not a fact about a
container format: remembering it separately for `.mp4` and `.mkv` would mean muting a
video and then being surprised by the next one. That is the argument
`PreviewTabController::setDetailsOpen()` already makes for the details panel, and this
is the second thing shaped like it — a choice about the person rather than about the
file.

**What this does not do is make autoplay conditional.** The muted state is remembered,
so somebody who wants silence gets silence from the second video onwards; the first
one after a fresh install plays with sound. Nothing here is a volume default that
survives being wrong, because it is the reader's own last answer rather than a guess
Mole made.
