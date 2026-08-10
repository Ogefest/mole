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

**Work lives on the Mole board in Vikunja** — a self-hosted instance on the
author's own network, not on GitHub. A bug, a feature, a test that is owed — each
is a task, and each should carry enough for somebody to pick it up without asking
a question: what is wrong or wanted, where in the code it lives, and how anyone
will know it is finished. A larger effort is an **epic**, which names what several
tasks add up to; all of it sits on one board. See
[ADR-0022](docs/adr/0022-work-is-tracked-in-vikunja.md) for why it moved, and
`~/dev/workspace/mole-pm/environment/vikunja.md` for the address, the account and
the token.

A task's number is the number the GitHub issue had before the move: `MOLE-19` is
what `#19` was, and the gaps in the sequence are the numbers that belonged to pull
requests. Every `Closes #…` already in the git history still names the right work.

An epic is three things, because Vikunja has no single feature that does the job:
a `milestone: …` label on each of its tasks, a column in the board's `By epic`
view, and a task in the `Epics` sub-project whose subtasks are the tasks
themselves. Put a new task in an epic by giving it the label; the other two follow.

There is no planning document in the repository. A task carries what a builder
needs and an epic names the effort it belongs to, and the reasoning that produced
either is planning work rather than engineering work — it happens before a task is
written, not beside it.

Before starting anything, look for the task. If there is not one, open it first —
a task written after the fact is a summary, and the point of writing it first is
that somebody else can see the work is taken.

A change that finishes a task says so in its commit message — `Closes MOLE-12`.
Nothing acts on that any more, so the card is moved by hand as well; the commit
message says it because the git history should record what a change finished.

**The repository is public. The board is not, and that changes nothing about what
may be written where.** No host names, no account names, no bucket names, no
credentials — not in the repository, and not on the board either, because a board
gets exported, backed up and read by tools. When a fault is reproduced against a
real server, the task describes the *behaviour*, not the address it was found at.

**The GitHub Issues tab is still open, and it is not the tracker.** It is the way
somebody outside can report a fault, since the board is unreachable from outside
this network. Anything that arrives there is copied onto the board and closed on
GitHub, so there is one queue rather than two.

**Those facts are kept, just not here.** What machine exists, how it is reached,
which account, where the credentials are — all of it lives outside this
repository, in `~/dev/workspace/mole-pm/environment/`. That now includes the board
itself: `vikunja.md` there holds the address and the only copy of the bot token. A
task labelled `needs-server` is one whose work depends on those facts, and that
directory is the first thing to read before starting one. Reading a path outside
this checkout may ask for permission the first time; that is the mechanism working.

Nothing comes back the other way. Not into a commit message, a code comment, a
test fixture, a task, a pull request, or a log attached to one. Anything the
code needs from there is a **parameter read at run time** — the way the
`MOLE_TEST_*` suites already take theirs from the environment. A host name typed
into a tracked file is not a small lapse to be tidied up later; it is the signal
that the code wanted a parameter and got a constant instead.

Outside the repository rather than in an ignored file inside it, because an
ignored file is one `git add -f`, or one careless edit to `.gitignore`, away
from being published. A directory that is in no git repository at all cannot be
pushed by any mistake.

The three files alongside it keep what a tracker holds badly:

- [CHANGELOG.md](CHANGELOG.md) — the short, user-facing list: **one sentence** per
  new feature or visible change, newest first. No design discussion, no
  internals, no restating the ADR.
- [DONE.md](DONE.md) — the long account of what was asked for and what the answer
  turned out to be, including the times the first answer was wrong.
- [TODO.md](TODO.md) — context that is *not* a task: behaviour we have decided to
  live with, conventions to follow, gaps that are documented rather than
  scheduled.

## Which task to take next

The board has five columns, and the one that matters is **Ready**: it holds
everything that can be picked up right now, in the order it should be picked up.
**Take the top card in `Ready`.** There is no choosing to do.

| Column | Holds |
|---|---|
| `Backlog` | not planned, or the brief is not complete enough to work from |
| `Ready` | planned and dispatchable, topmost card first |
| `In Progress` | being worked on now — one at a time |
| `Blocked` | waiting on something outside the repository, usually the live test environment |
| `Done` | landed and verified |

`Backlog` and `Blocked` are not a queue to dip into. A task leaves them in a
planning session, not because it looked convenient on the day.

Everything below takes the address and the token from the environment, never from
a file in this repository:

```sh
: "${VIKUNJA_URL:?}" "${VIKUNJA_TOKEN:?}"        # see mole-pm/environment/vikunja.md
v() { curl -sS -H "Authorization: Bearer $VIKUNJA_TOKEN" \
        -H 'Content-Type: application/json' "${@:2}" "$VIKUNJA_URL/api/v2$1"; }

root=$(v /projects | jq -r '.items[]|select(.title=="Mole")|.id')
view=$(v "/projects/$root/views" | jq -r '.items[]|select(.title=="Kanban")|.id')

v "/projects/$root/views/$view/buckets/tasks" \
  | jq -r '.items[]|select(.title=="Ready")|.tasks|sort_by(.position)[]
           |[.identifier,.title]|@tsv' | head
```

**Move the card to `In Progress` before writing a line of code** — not after the
first commit, not when the branch is pushed. It is the first step of picking a task
up, because it is the only signal anybody else has that the work is taken, and a
branch on one machine is not visible to anyone.

Move it to `Done` once the change is on `main`. Dropping a card in `Done` is what
marks the task done — the column is the view's done bucket, so there is nothing
else to remember. Nothing does this for you any more: there is no workflow watching
for `Closes MOLE-12`, which is exactly why it is the first and last step of picking
a task up.

A board that disagrees with the repository is worse than no board, because it is
believed.

```sh
board() {   # board <task-number> <column>
  local bucket task
  bucket=$(v "/projects/$root/views/$view/buckets" \
           | jq -r --arg c "$2" '.items[]|select(.title==$c)|.id')
  task=$(v "/projects/$root/tasks/by-index/$1" | jq -r .id)
  v "/projects/$root/views/$view/buckets/$bucket/tasks" -X PUT -d "{\"task_id\":$task}" \
    | jq -r '"\(.task.identifier) -> \(.bucket.title)"'
}

board 12 "In Progress"
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
