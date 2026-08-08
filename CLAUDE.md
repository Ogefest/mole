# CLAUDE.md

Working rules for this repository. They apply to every change, human or assisted.

## What this project is

**Mole** — a file manager for working with files at scale: an IDE, but for files.
Browsing, searching, comparing, syncing, bulk renaming, finding duplicates,
previewing anything, and running long jobs in the background, all under one
keyboard. Native and desktop-first, C++20 and Qt 6 / QML, with drives and tabs as
plugin extension points.

Always call it *Mole*. `superfilemanager` is only the directory name.

See [README.md](README.md) for what works today, [ARCHITECTURE.md](ARCHITECTURE.md)
for how it is put together, and [spec.md](spec.md) for the original intent.

## English only

English is the working language of the project, without exception:

- code, identifiers and comments
- strings shown to the user, log messages and error text
- commit messages and pull request descriptions
- documentation, ADRs, the changelog, TODO and DONE

This is an open source project — a contributor should never hit a wall of text
they cannot read. Conversation with the author may happen in any language; what
lands in the repository is English.

## Architecture decisions go in an ADR

Every significant change gets a record in [docs/adr/](docs/adr/) before or
alongside the code: choosing a library, adding or reshaping an extension point,
changing a file or config format, dropping a platform, taking on a dependency,
reversing an earlier decision.

One file per decision, `NNNN-short-title.md`, numbered in sequence and never
renumbered. Each one carries **the date** and **the reason the decision was
taken** — the alternatives considered and why they lost, so a reader a year from
now can tell whether the reasoning still holds. Superseding an ADR does not edit
it; write a new one and link back. See [docs/adr/README.md](docs/adr/README.md)
for the template.

Small changes — a bug fix, a rename, a test, a tidy-up — do not need one.

## Changelog

[CHANGELOG.md](CHANGELOG.md) is the short, user-facing list: **one sentence** per
new feature or visible change, newest first. No design discussion, no internals,
no restating the ADR — anyone who wants the reasoning can follow the link.

It is deliberately lighter than the other two records, which stay as they are:
[DONE.md](DONE.md) keeps the long account of what was asked for and what the
answer turned out to be, and [TODO.md](TODO.md) keeps what is left.

## Tests are part of the work

Tests are a first-class part of this project, not an afterthought. Every
feature is covered — a change that adds behaviour adds tests for it in the same
commit, and coverage of existing functionality is not allowed to regress.

**Every bug fix ships with a test that reproduces the bug.** Write the test
first, watch it fail for the reported reason, then fix it. A fix with no test is
not finished, because nothing stops the bug coming back.

Test layout mirrors `src/` under [tests/](tests/), with shared fixtures in
`tests/support/` — the backend conformance suite, the QML harness and the temp
tree helpers live there, so reach for them instead of copy-pasting scaffolding.

```
make test           # build and run the whole suite
make test-verbose   # same, printing every assertion
make asan           # address and undefined-behaviour sanitizers
```

The suite must be green before a commit.

## Housekeeping

- `make format` applies `.clang-format` to `src` and `tests`; `make tidy` runs
  clang-tidy over the compilation database.
- Match the surrounding code and prose. The documentation here is written in
  plain, explanatory English and the code is consistent about naming — follow
  what is already there rather than introducing a second style.
