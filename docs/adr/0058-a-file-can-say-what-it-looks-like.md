# ADR-0058: A file can say what it looks like, off the UI thread and optionally

- **Date:** 2026-08-20
- **Status:** Accepted

## Context

`grep -i thumb` over `src/` returned nothing. Mole could not make a small picture
of anything, which is why a folder of two hundred photographs in the gallery was
two hundred copies of the same emoji. The only way to tell one from another was
`F3` and the arrow keys.

Everything needed to fix that already exists in more than one place — `QImageReader`
for pictures, `QPdfDocument` for a first page, Qt Multimedia for a frame — and each
belongs to a different optional dependency. That is the shape of an extension point,
not of a function.

## Decision

**A fifth extension point, `IThumbnailer`, in the published SDK**, with a
`ThumbnailRegistry` in `host` reached through `PluginServices::thumbnails`. Three
rules, and they are the whole design:

- **Off the UI thread, always.** `thumbnail()` runs on a `TaskManager` worker like
  every other call that touches storage.
- **Bounded.** What comes back is at most `size` pixels on its longest edge, and a
  thumbnailer is expected to reach that without materialising the full image where
  the format allows it — `QImageReader::setScaledSize()` reaches libjpeg's own DCT
  scaler, so a 24-megapixel photograph is never decoded in full.
- **Optional by design.** A null image is an ordinary answer, not an error. An
  unplugged drive, a format nothing can decode, a corrupt file, a 400 MB TIFF: the
  tile keeps the icon it already had and nothing is put on screen about it.

**First match by priority, unlike the metadata readers.** Every reader that claims
a file contributes facts, because a container and its contents are two sets of
facts about one file. **A file has one picture**, so the highest-priority
thumbnailer that claims it answers and the rest are not asked.

**The picture reaches QML through a `QQuickAsyncImageProvider`** registered as
`mole-thumb`, and the url carries the whole key:

```
image://mole-thumb/<percent-encoded uri>?size=160&mtime=<seconds since epoch>
```

`mtime` is in the url because it is what makes an edited file produce a new
picture rather than the one from before, and it arrives free from a role the
listing already holds — so keying a thumbnail costs no extra `stat()`. `size` is
in it because the same file in the small grid and in the gallery are two different
pictures.

**`sdk` gains QtGui.** A thumbnail is a `QImage` and `QImage` is QtGui.

## Reason

The alternatives, and what disqualified them:

- **A synchronous `QImage thumbnailFor(uri)` called from the model.**
  `FileListModel`'s own comment says it does no I/O, and that is the rule that
  keeps a stalled NFS mount from freezing the window. A model that decodes is a
  model that blocks.
- **A `QQuickImageProvider` doing the work in `requestImage()`.** Qt calls that on
  its own pixmap reader thread, which would put decoding outside `TaskManager`
  entirely: no cancellation the rest of the application understands, no bound on
  how many run at once, and nothing in the task strip. The async provider is the
  only shape that keeps the work where every other long operation lives.
- **Baking the picture into the model as a role.** It makes the model the owner of
  a cache, a queue and a network policy — the three things MOLE-141, MOLE-142 and
  MOLE-143 are about — inside a class whose job is to hold rows.
- **Every thumbnailer that claims a file contributes, as with metadata.** There is
  nothing to combine. Two pictures of one file is a choice nobody can make
  usefully, and priority makes it once.
- **Keeping QtGui out of `sdk` by returning encoded bytes.** It would make every
  thumbnailer encode and every caller decode, for the sake of a dependency that
  brings no widget and no window — the whole suite still runs headless with it.

The url could have keyed on a content hash instead of the modification time. That
reads the file to decide whether to read the file, which is exactly backwards on
the remote drives this feature has to be careful on.

## Consequences

Adding a kind of thumbnail is a plugin, not a change to the shell: MOLE-144's PDF
and video thumbnailers are ordinary members of the registry, each present only
when its dependency is.

The Quick half is thin on purpose. Parsing the url, choosing the thumbnailer,
running the task and cancelling it live in `ui::ThumbnailPump`, which is headless
and tested; `app::ThumbnailImageProvider` is the shell around it. `mole_shell` is
a new static library in `src/app` so the headless harness loads the same provider
the window does — a provider registered only in `main.cpp` is one no test can be
wrong about.

`kPluginApiVersion` goes to 11, so a plugin built against 10 is refused with a
clear message rather than crashing on a vtable that grew.

Nothing yet caches, bounds the number of decodes in flight, or treats a remote
drive differently — those are MOLE-141, MOLE-142 and MOLE-143, and each is a
separate decision.
