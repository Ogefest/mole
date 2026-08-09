# CLAUDE.md

Working rules for this repository. They apply to every change, human or assisted.

## What this project is

**Mole** — a file manager for working with files at scale: an IDE, but for files.
Browsing, searching, comparing, syncing, bulk renaming, finding duplicates,
previewing anything, and running long jobs in the background, all under one
keyboard. Native and desktop-first, C++20 and Qt 6 / QML, with drives and tabs as
plugin extension points.

Always call it *Mole* — the name of the project and of its directory. The old
name `superfilemanager` survives only in older records, never in new work.

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

## Where work is tracked

**Work lives in [GitHub issues](https://github.com/Ogefest/mole/issues).** A bug,
a feature, a test that is owed — each is an issue, and each should carry enough
for somebody to pick it up without asking a question: what is wrong or wanted,
where in the code it lives, and how anyone will know it is finished. Larger
efforts are milestones over those issues, all of it on one
[board](https://github.com/users/Ogefest/projects/1), with a file in
[docs/projects/](docs/projects/) for the reasoning that will not fit in an issue.

Before starting anything, look for the issue. If there is not one, open it first
— an issue written after the fact is a summary, and the point of writing it
first is that somebody else can see the work is taken.

A change that closes an issue says so in its commit message, in GitHub's own
words — `Closes #12` — so the issue shuts when the work lands rather than when
somebody remembers.

**The repository is public, and so is every issue.** No host names, no account
names, no bucket names, no credentials — not in an issue, not in a comment, not
in a test fixture, however local the work felt while it was being done. When a
fault is reproduced against a real server, the issue describes the *behaviour*,
not the address it was found at.

The three files alongside it keep what a tracker holds badly:

- [CHANGELOG.md](CHANGELOG.md) — the short, user-facing list: **one sentence** per
  new feature or visible change, newest first. No design discussion, no
  internals, no restating the ADR.
- [DONE.md](DONE.md) — the long account of what was asked for and what the answer
  turned out to be, including the times the first answer was wrong.
- [TODO.md](TODO.md) — context that is *not* a task: behaviour we have decided to
  live with, conventions to follow, gaps that are documented rather than
  scheduled.

## Which issue to take next

The board has five columns, and the one that matters is **Ready**: it holds
everything that can be picked up right now, in the order it should be picked up.
**Take the lowest `Rank` in `Ready`.** There is no choosing to do.

| Column | Holds |
|---|---|
| `Backlog` | not planned, or the brief is not complete enough to work from |
| `Ready` | planned and dispatchable, lowest `Rank` first |
| `In Progress` | being worked on now — one at a time |
| `Blocked` | waiting on something outside the repository, usually the live test environment |
| `Done` | landed and verified |

`Backlog` and `Blocked` are not a queue to dip into. An issue leaves them in a
planning session, not because it looked convenient on the day.

```sh
gh project item-list 1 --owner Ogefest --format json --limit 200 \
  | jq -r '.items[] | select(.status=="Ready") | [.rank, .content.number, .title] | @tsv' \
  | sort -n | head
```

Move the card when the work moves: to `In Progress` on starting it, and to
`Done` once the commit has landed and closed the issue. A board that says
something different from the repository is worse than no board.

```sh
board() {   # board <issue-number> <column>
  local proj fid oid iid
  proj=$(gh project view 1 --owner Ogefest --format json | jq -r .id)
  fid=$(gh project field-list 1 --owner Ogefest --format json \
        | jq -r '.fields[] | select(.name=="Status") | .id')
  oid=$(gh project field-list 1 --owner Ogefest --format json \
        | jq -r --arg c "$2" '.fields[] | select(.name=="Status") | .options[] | select(.name==$c) | .id')
  iid=$(gh project item-list 1 --owner Ogefest --format json --limit 200 \
        | jq -r --argjson n "$1" '.items[] | select(.content.number==$n) | .id')
  gh project item-edit --id "$iid" --project-id "$proj" --field-id "$fid" \
    --single-select-option-id "$oid"
}
```

The order in `Ready` is deliberate rather than obvious. It is planned separately,
weighing what unblocks the most against what is losing data today, and the
reasoning does not fit on the card. If the top of the queue looks wrong from
inside the code — a dependency nobody saw, a fix that is three lines rather than
three days — say so rather than quietly taking something else. Being wrong about
the order is useful; working around it in silence is not.

## Tests are part of the work

Tests are a first-class part of this project, not an afterthought. Every
feature is covered — a change that adds behaviour adds tests for it in the same
commit, and coverage of existing functionality is not allowed to regress.

**Every fault becomes a test first, and is fixed second.** Write the test, watch
it fail for the reported reason, then fix it. A fix with no test is not
finished, because nothing stops the fault coming back.

"Fault" means any of them, not only the ones somebody reported. A stall found by
hand against a live server, a wrong answer noticed while looking at something
else, a race that only shows up under load — each one costs a day to find and a
second to keep, and the difference between a project that accumulates coverage
and one that keeps rediscovering the same bugs is entirely this rule.

**Put the test in the cheapest place that can hold it.** A fault found against a
real server should usually leave behind a test that does not need one: reproduce
the *behaviour* — a read that stops early, a listing that answers differently, a
write that is acknowledged and lost — through a fake or a wrapper, so it is
checked on every change rather than on the days somebody has a server to hand.
Keep the live test as well when the server is what is being doubted; the two
answer different questions.

**A test for a race waits for a condition, never for a clock.** Trigger on the
thing itself — a byte offset reached, a state entered — because a test that
sleeps for 200 ms passes on one machine and fails on another, and an
intermittent test is worse than no test: it teaches everyone to ignore red.

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
