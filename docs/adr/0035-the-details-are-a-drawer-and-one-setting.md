# ADR-0035: The details are a drawer, and whether it is open is one setting

- **Date:** 2026-08-11
- **Status:** Accepted. Supersedes part of
  [ADR-0034](0034-what-a-file-says-about-itself.md) — its extension
  point, its cost rule and its reader ordering stand; where it put the panel and
  how it remembered the answer do not.

## Context

ADR-0034 put the facts in a strip above the viewer and remembered whether it was
open the way ADR-0006 remembers a viewer's options: per file type, with a
per-viewer default through `IPreviewProvider::detailsOpenByDefault()`.

Three things were wrong with that once real readers arrived. A horizontal `Flow`
of label-value pairs is fine for three facts and unreadable for a photograph's
dozen — it has to be read like prose to find one line. The values were `Label`s,
so **nothing in the panel could be copied**: a camera model, a serial number or a
full path had to be retyped. And it sat above the viewer, so every row of it cost
the picture room whether or not anybody was reading it.

The per-file-type memory was wrong for a different reason: it is the right shape
for *render this `.html` as a page*, which is a choice about a kind of file, and
the wrong shape for a panel, which is a choice about a person and their screen.
Having the facts appear for a `.jpg` and vanish for a `.png` is the surprise, not
the service.

## Decision

**A drawer down the right-hand side**, inside a `SplitView`, with a `Details`
checkbox in the strip beside `Open`. The viewer keeps a minimum width and the
drawer a maximum share of the window, so turning it on narrows the picture rather
than replacing it. The divider drags and the width it is left at is remembered —
written when the divider is released, because a preference is a file on disk.

**One setting for every preview.** `preview.details.open` and
`preview.details.width`, not ADR-0006's per-suffix keys.
`IPreviewProvider::detailsOpenByDefault()` comes out of the SDK, which takes
`kPluginApiVersion` to 10.

**One list component, rendered in two places.** `DetailsList.qml` is the drawer
for every viewer, and the body of `FileInfoPreview.qml` for the one whose content
the facts *are* — a file nothing can show has nothing else to say, so it must not
go blank when the drawer is put away. That viewer asks for the facts itself
through `PreviewTabController::requestDetails()`, which is the special case the
removed virtual was really expressing.

**Values are selectable and the rows can be copied.** Each value is a read-only
`TextEdit` with `selectByMouse`, so a selection — including one that wrapped onto
two lines — leaves with `Ctrl+C`. A **Copy all** action puts every row on the
clipboard as `label: value` lines, from the controller, the shape
`TablePreviewController::copyBlock()` set.

**Rows keep their readers' order**, with a hairline where one reader's answer ends
and the next begins. `ReadMetadataTask` reports where each block started; nothing
is regrouped or reordered.

## Reason

**Why a drawer rather than a taller strip.** A list of facts is long and narrow.
Down the side it costs width, which a preview of a picture or a page has to
spare, and it scrolls on its own when a photograph's EXIF runs past the bottom.
Across the top it cost height, which is the dimension a picture and a page both
need.

**Why a checkbox rather than a `ViewerOption`.** An option belongs to a viewer and
is remembered per file type by ADR-0006's rule; this belongs to the tab and to the
person. Putting it in the same strip keeps it where the other switches are without
pretending it is one of them.

**Why one setting, against ADR-0006's grain.** Stated above: the panel is a
property of the reader's screen rather than of the file in front of them. This is
the first preference in the application that is deliberately *not* per type, and
saying why here is the point of the record.

**Why the information viewer asks rather than being asked about.** The alternative
was another virtual on `IPreviewProvider` — `rendersDetailsItself()` — which is
the same special case in the same place, one name later. A viewer that draws the
facts knows it draws them, and it is a view; asking the tab for what it is about
to render keeps the SDK smaller and the cost rule intact.

## Consequences

- ADR-0034's extension point, its "every reader contributes" rule, its priority
  ordering and its "nothing is read for a panel nobody opened" cost rule are all
  unchanged. Only the presentation and the preference are.
- A plugin built against `kPluginApiVersion` 9 is refused with a clear message; the
  method it would have overridden no longer exists.
- One more thing is remembered globally, which is a precedent to be careful with:
  the next preference that wants to be global has to make this argument again.
- The drawer is a `SplitView` pane, so the widths a small window can produce are
  bounded by the same mechanism the browser and the main window already use.
