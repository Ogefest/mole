# ADR-0001: Markdown previews are restyled in place, not converted

- **Date:** 2026-08-08
- **Status:** Accepted

## Context

The Markdown preview was correct and ugly. Qt's Markdown importer (Qt 6.4)
produces a document that reads as cramped, and measuring it shows why:

- a heading gets `topMargin` and `bottomMargin` of **zero**, so a section title
  sits flush against the paragraph above it,
- every block is set solid — no line height at all,
- a fenced code block arrives as **nine-point** monospace with no margins and no
  background, whatever size the surrounding prose is,
- inline `code` is nine-point too, which next to body text reads as a fault,
- tables get `cellPadding` of zero, so cell text touches the rules,
- and the view itself put the text edge to edge, with no margins and no cap on
  the line length.

Something had to give the rendered page the spacing prose needs.

## Decision

Style the real `QTextDocument` after the import, block by block, from a `Metrics`
struct holding one body size and a monospace family, with every other measure
expressed as a ratio of that size. Formats only — margins, line height, sizes,
weight, colour, cell padding. Never structure.

The styling is idempotent and is reapplied on every `contentsChanged`, because
setting a `TextArea`'s text re-imports the Markdown and discards everything done
here.

## Reason

Three other routes were tried or examined first.

**A style sheet.** `QTextDocument::setDefaultStyleSheet` is only consulted by the
HTML importer. `setMarkdown()` never looks at it, so this does nothing at all.

**Markdown to HTML, then CSS.** `setMarkdown()` followed by `toHtml()` and
`setHtml()` with a style sheet would put styling in one declarative place. It
fails on its own output: Qt writes explicit inline styles onto every block it
emits, and inline styles beat a style sheet. The round trip is also lossy for no
gain.

**Frames for code blocks.** A `QTextFrameFormat` is the only thing in Qt's rich
text that supports real padding behind a background, which would give code blocks
a properly padded band. Wrapping a fence in one was measured and rejected: a
four-block document became six, the visible text gained two blank lines, and
`toMarkdown()` came back with the fence broken. A preview has to show the file,
not a re-rendering of it — so the rule became formats only, never structure.

**Restyling in place** has none of those problems, at the cost of being
imperative code that has to be kept idempotent.

## Consequences

- The styling runs again on every change to the document, including the changes
  it makes itself. Everything it writes is an absolute value, never an increment,
  and the one signal it consumes destructively — the left margin that encodes how
  deeply a quote is nested — is recorded as a block property before it is
  overwritten. A test asserts that a second and third pass change nothing.
- Only one of the syntax highlighter and this may be attached to a document at a
  time. The highlighter answers every change with an overlay and this answers
  every change by rewriting the formats underneath it; on one document they would
  chase each other. `TextPreviewController` attaches whichever the current file
  needs and detaches the other.
- The scene graph behind a QML `TextArea` paints character backgrounds and
  ignores block backgrounds, so the slab behind a code block has to be a
  character background. That in turn means code is set solid: extra leading falls
  outside the painted area and cuts the band into stripes.
- Quirks in the importer itself are out of scope, because fixing them would mean
  editing the document's structure. The known one: a blockquote and a fenced code
  block each end with a stray empty block, and if a table follows either one that
  block lands inside its first cell — so the header loses its bold and gains an
  empty line. This happens before any of this code runs.
