# ADR-0006: Viewers declare their options; preferences remember them per file type

- **Date:** 2026-08-08
- **Status:** Accepted

## Context

An `.html` file previewed as coloured source. Sometimes that is exactly what
someone wants and sometimes they want to see the page, so it is a setting rather
than a decision — and there was nowhere to put a setting. `SessionStore` persists
the open tabs and the window geometry; nothing else was remembered about anything.

Three questions had to be answered before code: where preferences live, how the
strip above a preview learns what options a viewer has, and what a remembered
choice is keyed by.

## Decision

**Preferences live in one small store.** `Preferences` reads and writes a single
JSON file of dotted keys — `preview.mole.preview.text.html.mode` — with the same
env-var override every other store here has (`MOLE_PREFERENCES_PATH`), so tests
never touch the developer's own. It knows nothing about what the keys mean.

**Viewers declare their options; the strip renders them blind.** An
`IPreviewProvider` may return a list of `ViewerOption`s for a given entry: a key, a
title, the choices, and which choice is the default. The preview strip renders them
without knowing what any of them mean, the same way the menu renders entries
contributed by plugins it has never heard of. The chosen value reaches the
controller through `PreviewController::setViewerOption()`, whose default
implementation ignores it.

**A choice is keyed by provider and suffix.** `preview.<providerId>.<suffix>.<key>`.
Per suffix because that is what the request describes — *the next `.html`* — and
because one viewer serves many suffixes that want different answers: rendering an
`.html` is useful, "rendering" an `.xml` is not. The provider id is in the key so
two viewers that both claim a suffix cannot overwrite each other's answer.

**A rendered document fetches nothing.** HTML handed to the view is stripped of
anything that could reach off the disk — `img`, `script`, `link`, `iframe`,
`object`, `embed`, and `on*` handlers — before it is rendered.

## Reason

Keying per suffix rather than per provider was the close call. Per provider is what
the strip is showing, and it is fewer keys; but it means choosing "rendered" for one
`.html` silently changes how `.xml` and `.svg` open, since one text viewer claims all
three. The request was about *the next `.html`*, and the narrower key is the one that
cannot surprise anyone. The cost is that someone who wants every kind of markup
rendered has to say so once per kind, which is a fair price for never being
surprised.

Declared options rather than a viewer-specific strip: the alternative was for the
preview view to know that the text viewer has an HTML mode, which puts knowledge of
one viewer into the shell and guarantees the next viewer's options need shell changes
too. Providers already declare their name, their priority and their view; declaring
their options is the same idea.

The no-network rule is not a nicety. Qt's rich text engine resolves the resources a
document names, so a page could quietly tell whoever wrote it that a file was looked
at, and in a file manager that is a nasty surprise rather than a feature. Stripping is
cruder than a resource-loading policy and it is certain, which is the right trade for
something that must not happen: previewing a file puts nothing on the network. What is
shown is the document's text and structure, not its pictures.

## Consequences

- Preferences are written on change, whole file each time. It is a handful of keys;
  when it stops being that, it can grow a flush timer.
- A remembered option applies from the *next* time a file of that type is opened as
  well as immediately, because choosing it re-loads the current file through the same
  path. Nothing has to be reopened by hand.
- A viewer with no options is unaffected: the strip shows nothing extra and the
  controller never hears about any.
- Rendered HTML shows no images at all, including local ones. That is deliberate
  bluntness — telling a local `img` from a remote one means parsing and resolving the
  document's references, and getting that subtly wrong is exactly the failure this
  rule exists to prevent. If local images become worth having, they get their own
  decision.
