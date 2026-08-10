# ADR-0034: What a file says about itself is a fifth extension point

- **Date:** 2026-08-11
- **Status:** Accepted

## Context

Everything Mole said about a file was nine facts built by hand in
`FileInfoPreviewController::load()`, out of `stat()` and the file's name — and
they were only shown when *no* viewer claimed the file. So a photograph showed
its pixels and not its dimensions or its camera, a PDF rendered its pages and
never said who wrote it, and a `.docx` got nine numbers although the author's
name is a few hundred bytes of XML inside it.

The facts are per format, so they belong to something registered per format
rather than to the shell. They also have to reach a file that already has a
viewer, which none of the four existing extension points can do: a preview
provider is chosen instead of every other provider, and the chosen one is busy
showing the file.

## Decision

**`IMetadataReader`, a fifth extension point**, in `src/sdk/`, with
`MetadataRegistry` in `src/host/` reached through `PluginServices::metadata` the
way `IPreviewLookup` is. A reader answers with a list of `FileFact` — a label and
a value, both already in the form they are shown in.

Four rules make it what it is:

1. **Every reader that claims a file contributes.** The registry returns all
   matches in priority order rather than the first one.
2. **A reader is given the head and asks for the rest itself.** `read()` receives
   the 4 kB the type sniff already read (ADR-0033), the services, and the running
   task's cancel token. Nothing reads a whole file.
3. **`canRead()` does no I/O**, for the same reason `canPreview()` does not: it is
   asked of every reader for every file.
4. **The facts are read when the panel is opened**, not on every preview.

**The panel lives in `PreviewView.qml`**, filled by the tab controller from the
registry. No viewer knows it exists and every viewer has it. Whether it is open
is remembered per file type through `Preferences`, keyed the way ADR-0006 keys a
viewer option, and a viewer may say what an unanswered file type gets through
`IPreviewProvider::detailsOpenByDefault()`.

**The information viewer keeps its headline and loses its facts** to the one
reader shipped here — the generic one, which claims every file and reports what
`stat()` and the sniffed type know. It is also the viewer that opens the panel by
default, because there the details are the whole content.

`kPluginApiVersion` goes to 9.

## Reason

**Why not extend `IPreviewProvider`.** Facts would then belong to whichever viewer
won, so a `.docx` shown as bytes would have no author and a photograph shown by a
plugin's viewer would lose its EXIF. What a file *is* and how it is *shown* are
different questions, and tying them together is what produced the fault in the
first place.

**Why every reader contributes, unlike the preview lookup.** A file is shown one
way — two viewers cannot both draw the pane — so `PreviewRegistry` has to pick.
Facts do not compete: a container and its contents are two sets of true
statements about one file, and a zip reader saying "37 entries" and a document
reader naming the author are both right at once. Making them fight would mean
one of them silently losing.

**Why on demand rather than on every preview.** Stepping through a folder with
`←`/`→` opens a file per keystroke, and a reader that decodes a video header
would make that crawl. Reading when the panel is open puts the cost on whoever
asked for it, and leaves a preview exactly as cheap as it was.

**Why per file type rather than one global switch.** The same argument ADR-0006
makes for a viewer's options: somebody who wants EXIF on every photograph does
not want a details panel on every log. It is the same key shape, so there is one
convention to learn rather than two.

**Why a reader that throws is caught.** The panel is a set of independent claims
about a file. One plugin's bad afternoon costs its own rows; taking the others'
answers with it would make the whole panel as reliable as its worst reader.

**Why no exiv2, which the roadmap promised.** The first reader written against
this interface is the image one, and the obvious way to write it is to link
exiv2. It is not taken. A reader is handed a *prefix* of a file that may be on a
remote drive, and exiv2 wants a path or a whole buffer — so using it would mean
either fetching a 60 MB raw file to read forty bytes of it, or reimplementing the
bounded read underneath it anyway. Two IFD walks over a checked buffer are two
hundred lines, they work identically over SFTP and inside an archive, and they
add no packaging to every platform Mole ships on. If XMP, IPTC and forty raw
formats are ever wanted, exiv2 is the upgrade path and this interface does not
change.

**Why the facts are strings.** A reader knows whether a number is seconds,
samples or frames, and the panel does not. Handing over "4:32" rather than 272
puts the formatting where the knowledge is, and keeps the panel free of a type
system for units it would then have to grow for every new reader.

## Consequences

- A plugin can describe a format without shipping a viewer for it, and a viewer
  can be added without repeating the facts.
- `PreviewServices` grows a field and the plugin API a method, so
  `kPluginApiVersion` is 9 and a plugin built against 8 is refused with a clear
  message rather than crashing.
- The nine generic facts now reach every file, including the ones with viewers of
  their own — which is the fault this fixes.
- A reader that wants more than the head does its own bounded read. Nothing
  enforces "bounded" but the contract and the review; a reader that reads a whole
  file will be as slow as it deserves, on a worker thread, and cancellable.
- The tickets after this one — EXIF, document authors, media tags — are each one
  reader and no shell change at all.
