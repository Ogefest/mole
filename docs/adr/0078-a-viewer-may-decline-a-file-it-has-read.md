# ADR-0078: A viewer may decline a file it has read, and the decline steps down the ladder

- **Date:** 2026-08-22
- **Status:** Accepted

## Context

A file gets its viewer from one question. `IPreviewProvider::canPreview()` is a
cheap test on the name, the suffix and the size, and it **must do no I/O** — the
registry asks it of every provider in priority order for every file somebody
looks at, and a lookup that read from a drive would make walking a folder with
the arrows a walk over the network.

So the only admissibility question in the system is asked before anything has
been read. Everything that decides what showing a file will actually cost, or
whether it can be shown at all, is in the bytes:

- **A container this build can demux may hold a stream it has no decoder for.**
  Nothing knows until the player tries.
- **A `.png` may not be a PNG**, or may be truncated, or may be a variant this
  build's image plugin does not handle. The suffixes the image viewer claims are
  the ones Qt says it has plugins for; whether a particular file decodes is a
  different question.
- **A 238 kB Markdown file may cost 2,676 ms in one uninterruptible call**, if
  its largest table is 2,182 rows (MOLE-283). Its size says nothing about that:
  235 kB of prose costs 122 ms.

By the time any of that is known the viewer is already the one on screen, holding
the thread that draws. Before this record its options were an error message in an
empty pane or a window that stops answering, and each viewer invented its own:
the video viewer put a sentence in a black frame, the image viewer put one in an
empty one, and MOLE-112's answer for a file with no line breaks in it — fold the
lines, turn the colouring off, say so in the strip — was written for one viewer
and one shape of file.

The requirement is that **the window stays answerable whatever it is pointed
at**: at worst the preview says it cannot show this file, or falls back to
something plainer, and it never takes the application with it. That is a rule
about the preview tier as a whole, not about any one viewer, so it is worth
stating once rather than being reinvented per fault.

## Decision

**A second admissibility question, asked of the bytes.** `canPreview()` is
unchanged. A viewer that has read its window and finds it cannot show the file
calls `PreviewController::decline(reason)`, and a decline has a defined outcome
rather than being the viewer's own business:

- **The file goes to the next viewer down the ladder.** `IPreviewLookup` gains
  `providerBelow(entry, above)`, which is `providerFor()` resumed from a position
  rather than from the top. The ladder is the registry's own order — the one the
  first choice came from — so a plugin that inserts a viewer into it is stepped
  through like anything else.
- **The strip says which viewer gave up and why.** `PreviewTabController`
  exposes `fallbackNote`, in the colour of a caveat and beside the name of the
  viewer that ended up with the file.
- **The bottom of the ladder cannot decline anywhere.** The information viewer
  accepts every file and has nothing below it, which is what makes the walk
  terminate: each step is strictly one place down a finite list.

`decline()` also sets `errorText`, so a viewer built by a test — or by a host
that has not implemented the fallback — still says what happened somewhere a
person would see it.

**Falling back is not the only shape of a decline.** Where the *file* is
showable and only one *way of showing it* is not, the viewer declines to itself:
MOLE-283's Markdown budget shows the source instead of the page and offers
`Show: Rendered` for a reader who wants it anyway. That is a mode of the viewer
rather than a step down the ladder, and it is the better answer whenever the
viewer still has something true to show. The ladder is for when it has nothing.

## Reason

**Why the ladder rather than each viewer choosing its own fallback.** A viewer
that picked its own replacement would need to know what else exists, which is
exactly what the registry is for and exactly what a plugin's viewer cannot know.
The priorities already encode the answer — a dedicated viewer, then the text or
hex viewer, then the list of facts — and it is the same order the file was
offered around in the first place. Two orders that could disagree would be worse
than one.

**Why one place down and not one tier down.** Three viewers sit at priority 60
today: the database, Parquet and video. A decline defined as "the next lower
priority" would skip two viewers that might have shown the file. So
`providerBelow()` walks positions in the sorted list, not numbers.

**Why the reason is shown rather than logged.** A preview quietly showing the
facts about a video looks exactly like a preview that never tried to play it,
and a reader who came to watch something would conclude the file was fine and
Mole was pointless. The note is the difference between a fallback and a failure
nobody mentioned.

**Why not off the GUI thread instead.** For some of these it is the right answer
and it is not this one. `QQuickTextEdit` owns the document behind
`textFormat: MarkdownText`, so there is nowhere to hand a document built
elsewhere; a media pipeline is already off the thread and still cannot say in
advance whether a stream will play. Where a viewer *can* move its work off the
GUI thread it should, and that is a ticket per viewer rather than a reason to
have no answer for the ones that cannot.

**Why not a `mightBeExpensive()` on the provider.** It was considered: a third
virtual, asked after a sample had been read, answering before a controller
exists. It loses the thing that makes a decline useful — the viewer declines
having read *its own window in its own way*, which is the only reading that
knows what that viewer is about to do with it. A provider answering from a
sample would be guessing about its own controller.

**Why not refuse rather than fall back.** "Nothing can show this file" is
already what the pane says when no provider claims a file, and it is the wrong
answer here: something can. A video with no decoder still has a duration, a
codec and dimensions, and every one of those is more use than an empty frame.

## Consequences

- A viewer written next year gets this by calling one protected method. It does
  not learn what is below it, and it cannot get the fallback wrong.
- The video and image viewers now hand a file back instead of showing a message
  in an empty pane. `VideoPreviewController::reportPlaybackFailure()` and
  `ImagePreviewController::reportDecodeFailure()` are the two call sites, both
  reached from the view, because in both cases the failure is the QML element's
  to notice.
- A decline arrives from inside the viewer's own read, so the tab acts on it
  through a queued connection: the object about to be deleted is the one whose
  call is still running.
- A decline is one-way. There is no stepping back up, and a viewer that declines
  a file will be asked again the next time somebody opens it — which is right,
  because the answer can change when a codec is installed.
- **A viewer that declines wrongly hides itself.** The image viewer's decline is
  guarded on having a source at all, because a QML `Image` reports `Error` for an
  empty one too, and a decline in that gap would send every image in the
  application to the list of facts. Any new decline needs the same care, and a
  test that a viewer does *not* give a good file up is as important as one that
  it gives a bad one up.
- Nothing about `canPreview()` changes, so no provider outside this tree has to
  be touched, and the no-I/O rule that keeps folder walking cheap still holds.
