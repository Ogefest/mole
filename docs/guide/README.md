# Using Mole

An IDE, but for files. One window in which to browse, search, compare, rename in
bulk, look inside anything and run long jobs in the background — without reaching
for a terminal or a second application.

This guide describes what works today. Nothing here is aspirational: **every picture
in it was taken by the test suite immediately after asserting that the state it shows
is real**, so a screenshot cannot quietly outlive the feature it documents. If a
picture is here, something checks it.

| | |
|---|---|
| [Browsing](browsing.md) | panes, tabs, the gallery, the keyboard, folder sizes, git state, how it looks |
| [Looking inside files](previews.md) | text, code, Markdown, tables, PDFs, HTML, images, databases |
| [Finding things](searching.md) | searching a tree, the indexes you have, doing something with the results, and the sets you keep them in |
| [Operations](operations.md) | compressing, dragging files in and out, renaming in bulk, the terminal, analysis, duplicates, sync, alerts |
| [Network drives](drives.md) | SFTP, FTP, S3, WebDAV, the passphrase that opens them, and what one drive can do that another cannot |
| [One key for everything](palette.md) | the command palette |

## Getting it running

```sh
make            # build
make run        # build and launch, keeping a log of the session
make test       # the whole suite; it is expected to be green
```

`make bundle` produces a self-contained folder in `dist/` that runs on a machine
without Qt installed.

## Staying up to date

Mole is in no distribution's archive, so nothing updates it for you. When it starts
it asks once whether a newer version has been released, and if one has it says so at
the bottom of the window, with a button that opens that version's page.

- **Once per version.** You hear about 0.2.0 once. Dismissing the notice counts as
  having read it — there is no *remind me later*, and it is not shown again.
- **Then a week of quiet.** If you do nothing, Mole does not even ask for seven days,
  so a version released two days after you were told is not found until the week is
  out. That is deliberate: being asked the same question every morning is worse than
  hearing about a release a few days late.
- **Nothing is downloaded and nothing is installed.** The notice names the version
  and opens its page; installing it is whatever you did the first time.
- **Help → Check for new versions** turns the whole thing off, the request included.
  Unticking it changes nothing else: what you have already been told stays told, and
  ticking it again does not force a check — the week of quiet still applies.
- Offline, behind a proxy or behind a firewall, nothing happens at all: no message,
  no warning and no delay.

[README.md](../../README.md#checking-for-new-versions) has the exact URL and the
exact headers, which is what to read if the question is what leaves the machine.

## The shortest possible tour

![The browser](images/01-browser.png)

A drive list on the left, tabs across the top, and a listing in the middle. The
search box in the title bar is the one thing worth learning first: **`Ctrl+R`** opens
it, and it can reach every command, bookmark and drive by typing part of its name.

## Keeping this guide honest

The pictures come from `make screenshots`, which drives the real application
headlessly and photographs each state the walkthrough test has just asserted. To
refresh them after a change:

```sh
make guide-images
```

That regenerates them and copies them into `docs/guide/images/`. A picture that
changes is a picture whose feature changed; a picture that vanishes is a test that
stopped taking it.
