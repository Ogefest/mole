# ADR-0084: The newest release is stated in a file at a fixed path

- **Date:** 2026-09-01
- **Status:** Accepted

## Context

`v0.1.0` is published and nothing in Mole will ever mention that `v0.2.0` exists.
Everybody who has Mole installed it by hand — it is in no distribution's archive —
so the only way anybody finds out about a new version is by going back to the
repository and looking.

There is no shortage of places that *state* the version. The release page says it in
HTML, `git tag` says it to somebody with a checkout, `CMakeLists.txt` says what the
build is at. None of them is something a running application can ask, which is the
whole of the problem: the question is machine-readable and every existing answer is
not.

`MOLE-323` is the file that answers it. `MOLE-324` is what reads it, `MOLE-325` the
notice and the switch, `MOLE-326` what says so in the documentation.

## Decision

**A JSON file at `latest.json`, at the top of the repository, written by
`scripts/release.sh` in the release commit.** It is fetched over
`https://raw.githubusercontent.com/Ogefest/mole/main/latest.json`.

```json
{
  "format": 1,
  "version": "0.1.0",
  "released": "2026-09-01",
  "url": "https://github.com/Ogefest/mole/releases/tag/v0.1.0"
}
```

- **`format` is an integer, and fields may only ever be added.** Renaming or
  removing one breaks every copy of Mole already installed — and coping with a
  change is exactly what those copies cannot do, because coping is what the update
  check is for.
- **`url` is the landing page, and the application opens what it is handed.** It
  never assembles a URL of its own. That is the entire reason the field exists:
  pointing releases at a real landing page one day is an edit to this file and no
  change to any binary already in the world.
- **`released` is the day the tag was cut**, in the shape `CHANGELOG.md`'s own
  marker uses. `release.sh` reads the clock once and uses it for both, so the two
  cannot disagree across midnight.
- **The path can never move.** Every binary that ships from now on asks for that URL
  for the rest of its life. It is the one decision in this record that is not
  revisable, and it is written into `release.sh` beside the file it writes.

The commit that introduces the file writes it once for `0.1.0`, which is already
published and whose date the changelog already states. Every write after that is
`release.sh`'s, in the same commit that carries the version and the changelog
marker.

## Reason

**`raw.githubusercontent.com` rather than the GitHub API.** Measured on 2026-09-01:
`raw` answers with `cache-control: max-age=300` and an `ETag`, so the check is a
conditional `GET` that comes back `304` with no body almost every time. The API
answers the same question and carries `x-ratelimit-limit: 60` **per IP per hour**
unauthenticated — a few dozen people behind one NAT starting Mole in the morning
would exhaust it, and the check would then quietly stop working for all of them.
Quietly is the part that disqualifies it: the failure would look exactly like being
up to date.

**A file in the repository rather than anything that has to be kept alive.** A
manifest on a web host, a `gh-pages` branch, a redirector — each is one more thing
that can be down, expire, or be forgotten when somebody else takes the project
over. This file is in the same push as the tag it describes, so it cannot rot
separately from the release.

**The repository root rather than `docs/` or `.github/`.** The file is a fact about
the repository, not about its documentation or its automation, and the URL is short
enough to read out loud or paste into a ticket. `docs/` would put it behind a
decision about documentation layout that we should stay free to change; the whole
point is that this path is the one thing that cannot change.

**Written by `release.sh` and not by the release workflow.** `release.sh` is already
the only thing in the repository that makes a tag (`MOLE-118`), and the manifest
belongs in the same commit as the version and the marker for the same reason they
belong together: one commit that is the release, or none. A workflow writing it
after the tag would need a second commit to `main` from a runner, which is both more
machinery and a race with whoever pushes next.

**No comment field.** The file has nowhere to explain itself, because a field may
never be removed and prose committed forever is prose that goes stale forever. The
explanation is here and in `release.sh`.

The alternatives that lost:

- **Read the version out of a release's assets or its tag list.** Same API, same
  rate limit, and it also makes the application decide what "newest" means — a
  pre-release, a draft, a tag that was pushed by hand. The file says it, so nothing
  has to infer it.
- **Scrape the release page.** HTML nobody promises us, and a check that breaks on a
  redesign.
- **A version number in a plain text file.** It parses more easily and answers less:
  there would be nowhere to put the landing page, and nowhere to put the next field
  either. `format` plus a JSON object is what makes the file able to grow.
- **A digest of the artefacts in the same file.** Wanted eventually, and not now.
  Adding it later is one more field, which is what the format was chosen for.

## Consequences

- **`latest.json` is a permanent commitment.** It may never move and its fields may
  never be renamed or removed. Somebody wanting a different shape adds a field, or
  bumps `format` and keeps answering the old one.
- **There is a window in which the file names a release that does not exist.** It is
  written in the release commit, and the tag is pushed a few seconds later at the
  end of the same script. `release.sh` says so in a comment rather than engineering
  around it; a reader who fetches in that window gets a version they cannot download
  yet, and gets it right on the next check.
- **A failed release leaves it alone.** `release.sh` puts the tree back on any
  failure after its first write, and this file joined that set — which is where the
  restore had to stop being a single `git checkout` of every path at once, because
  such a checkout is refused *whole* when one of its paths is not yet in `HEAD`, and
  on the cut that introduces the manifest that is the manifest. It would have put
  nothing back at all: not the changelog, not the pictures. `tst_Release.sh` holds
  both halves.
- **Nothing downloads or installs anything.** Mole says a version exists and opens a
  page. A file manager that rewrites its own binary is a different application with
  a different threat model.
- **A fork inherits the URL.** A binary built from a fork asks this repository what
  the newest version is unless whoever forked it changes the constant. That is the
  right default for a fork that means to stay in step, and it is one string for a
  fork that does not.
