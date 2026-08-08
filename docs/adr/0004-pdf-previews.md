# ADR-0004: PDFs are rendered with Qt PDF, a page at a time, through files

- **Date:** 2026-08-08
- **Status:** Accepted

## Context

A PDF fell through to the information viewer — the last resort for a file we
cannot show — so previewing one gave its size and its type and nothing of its
contents, while the listing already drew it its own icon and
`src/sdk/IPreviewProvider.h` named PDF in its own description of what previewing
is for.

Two things had to be decided before any code: what renders the pages, and how a
rendered page reaches the screen.

## Decision

**Qt PDF renders.** `QPdfDocument` from the `Qt6::Pdf` module, an optional
dependency: found at configure time it defines `MOLE_HAVE_QTPDF` and the provider
is compiled in; absent, the provider is not, and a PDF lands on the information
viewer exactly as it did before.

**Pages reach the screen as image files.** The controller renders a page to a PNG
in a scratch directory on request and hands QML a `file:` url. Pages are rendered
when they are asked for, not up front.

**Read-only, and nothing off the network.** The document is opened for reading and
never written back, as with the SQLite and Parquet viewers. Qt PDF resolves
nothing remotely, and no code here gives it the chance.

## Reason

**Why not poppler.** poppler is GPL-licensed, which does not sit with shipping
Mole under Apache-2.0. Qt PDF as packaged here declares `LGPL-3 or GPL-2` for the
module itself, which satisfies the condition the audit in
[docs/LICENSING.md](../LICENSING.md) turns on: every Qt module used is LGPL, and no
GPL-only module is referenced. It is dynamically linked like the rest of Qt.

Qt PDF embeds PDFium, which brings its own third-party components under permissive
terms (BSD-3-Clause, Apache-2.0 and others). Those live inside `libQt6Pdf.so` and
not in our binary, but they are now part of what a bundle ships, so
[THIRD-PARTY-NOTICES.md](../../THIRD-PARTY-NOTICES.md) records the module and what
it carries, and `make licence-check` has been re-run.

**Why not `QtQuick.Pdf`.** Qt ships `PdfMultiPageView`, which would have been most
of this view for free — and its QML module is not installed here
(`qml6-module-qtquick-pdf` is available but absent), so relying on it would mean a
second optional dependency for the same feature and a view that silently does not
exist without it. Rendering through `QPdfDocument` costs a page-image path and buys
back control over when pages are rendered, which is the part that matters for a
six-hundred-page scan.

**Why files rather than a `QQuickImageProvider`.** An image provider is the
idiomatic route and is registered on the `QQmlEngine` — which the application owns
and a preview provider, by design, cannot reach. Threading it through the plugin
boundary to save a temporary file would trade a real architectural rule for a
smaller one. A scratch directory is already how a preview gets at a file on a
remote drive (`LocalCopyProvider` exists because an `<Image>` cannot open
`archive://`), so this follows a path the code already has.

## Consequences

- A page is rendered when a delegate asks for it, on the thread that asks. That is
  tens of milliseconds for a typical page, so scrolling fast through a long
  document can hitch. Accepted for now, and the shape of the fix if it becomes
  annoying is known: move the render into a `Task` and let the delegate show a
  placeholder until it lands.
- Rendered pages cost disk in a `QTemporaryDir` that goes when the preview closes.
  One file per page and width, so re-reading at the same width is free and a
  different width re-renders.
- A PDF on a remote or archive drive is copied locally first, through
  `LocalCopyProvider`, because a renderer needs a file.
- In a build without `Qt6::Pdf` the provider does not exist and PDFs behave as they
  did. The tests state which of the two builds they are asserting, so a build
  without the module is still a green build rather than a skipped one.
