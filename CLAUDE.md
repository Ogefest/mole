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
- documentation, ADRs, the changelog and TODO

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

**Every task belongs to exactly one epic**, and an epic is three things, because
Vikunja has no single feature that does the job:

- an `epic: …` label on each of its tasks — this is the one that decides
  membership, so putting a new task in an epic means giving it the label
- a column in the board's `By epic` view, which filters on that label and so
  follows on its own
- a card in the `Epics` sub-project, whose subtasks are the tasks themselves. This
  is where the epic's own brief lives, where its progress is counted, and **where
  the order of the work is decided** — see below

A task belonging to nothing would be a task nobody ever reaches, which is why there is
always a **catch-all** epic: the one for a single fault or a single small change that is
not part of any larger effort. The `no epic` column of the `By epic` view should
always be empty, and a card appearing in it means one fell out.

**The catch-all is a batch, and its name changes.** It fills, gets worked, closes when its
last task lands, and planning creates the next one when the next small thing arrives. `Loose ends`
was the first and closed on 2026-08-19; `Loose ends II` was made and closed the same day;
`Loose ends III` did twenty tasks and closed on 2026-08-21; `Loose ends IV` follows it. **So never
type the name from memory and never attach a task to a finished epic** — that is what happened to `Testing: phase 6 — automation`, whose card was closed while
`MOLE-29` stayed open, and the task was unreachable for eight days because the queue is the epics
and a finished epic is never offered.

**Much of the time no catch-all is open at all**, because one closes the moment its last task lands
and the next is made only when something needs it. That is the normal state and not a fault. **As of
2026-08-22 `Loose ends V` is open**; `Loose ends IV` before it held the four tasks you opened while
working the batch before *that*. Never take any of it from this page, though: read the open titles,
because a batch can close between one task and the next — `Loose ends III` closed while its last four
tasks were being promoted, and they were moved into `IV` rather than left under a finished card. When
you open a small task and the open-epic list holds no catch-all, **leave the task with no `epic:`
label and say so in the message you send.** Planning sees it in the `no epic` column, which is
exactly what that column is for, and makes the next batch. An unlabelled task waiting an hour is
recoverable; a task filed into a finished epic is invisible, and that is the failure above.

**The catch-all is the last resort and not the default.** It was broken up on
2026-08-18 after growing to eighteen open tasks over six unrelated subjects, and rotated on
2026-08-21 for the other reason a batch goes wrong: `Loose ends III` kept producing a new task from
nearly every one worked, so its card never converged. **A batch you keep adding to is worth saying
out loud in the message you send**, because two tasks in one subject is an epic planning should make
rather than a batch that grows. A
fault found in a transfer belongs in the transfer epic, one found in a preview in
the preview epic; reach for the catch-all only when no epic fits. The open titles are one
call away, and reading them costs nothing — note `select(.done|not)`, which is what keeps a
finished epic out of the answer:

```sh
v "/projects/$epics/tasks?per_page=50" | jq -r '.items[]|select(.done|not)|.title'
```

**You write to the board as yourself, and in this checkout you are the engineering
role.** There is an account for planning and an account for engineering, each with
its own credentials, so who moved a card and who left a comment is a fact on the
board rather than a guess. The role follows the directory, so there is nothing to
decide and nothing to set up first — one line, at the start of any shell that talks
to the board:

```sh
set -a; . ~/dev/workspace/mole-pm/environment/vikunja/engineer.env; set +a
```

That is the only place the address and the token come from. **Never a value typed
into a command** — that writes the credential into the session transcript — and never
anything in this repository, which is public. Everything below then works from
`$VIKUNJA_URL` and `$VIKUNJA_TOKEN`.

One consequence to know about, because the error is a bare `403`: a label is only
usable by an account that can already see it in use somewhere. Every label the
board needs is in use, and a brand-new one has to be introduced by the planning
account — which is the right way round, since inventing vocabulary is planning.

There is no planning document in the repository. A task carries what a builder
needs and an epic names the effort it belongs to, and the reasoning that produced
either is planning work rather than engineering work — it happens before a task is
written, not beside it.

Before starting anything, look for the task. If there is not one, open it first —
a task written after the fact is a summary, and the point of writing it first is
that somebody else can see the work is taken. Opening one is four things, and a
task missing any of them is one nobody will ever be given:

```sh
# v(), $root and $epics come from "Which task to take next" below.
# The body is Markdown in a file — quoting it through a shell mangles it.
id=$(jq -n --arg t "A cancelled task is logged as a failure" \
        --rawfile d "$SCRATCH/task.md" '{title:$t, description:$d}' \
     | v "/projects/$root/tasks?format=markdown" -X POST -d @- | jq -r .id)

# The epic this belongs to. The catch-all only when nothing else fits -- see above.
# Read the title from the board; do not type it from memory.
epicTitle="<one of the open titles printed above>"

# The card, and it must be an OPEN one. Empty means the epic is finished or absent:
# stop, do not fall back to another epic, and see below.
epic=$(v "/projects/$epics/tasks?per_page=50" \
       | jq -r --arg t "$epicTitle" '.items[]|select(.done|not)|select(.title==$t)|.id')
[ -n "$epic" ] || echo "no open epic called $epicTitle -- read the note below"

lab() { v /labels | jq -r --arg t "$1" '.items[]|select(.title==$t)|.id'; }
for t in bug area:vfs "epic: $epicTitle"; do        # what it is, where it lives, its epic
  v "/tasks/$id/labels" -X POST -d "{\"label_id\":$(lab "$t")}"
done

v "/tasks/$epic/relations" -X POST \
  -d "{\"relation_kind\":\"subtask\",\"other_task_id\":$id}"
```

**If no catch-all epic is open**, which happens as a matter of course once one closes:
open the task, give it its `what it is` and `where it lives` labels, and **leave it with no
`epic:` label at all**. Say so when you hand back. It will show up in the `no epic` column,
which is the alarm built for exactly this, and planning creates the next batch for it. That
is the right outcome — **filing it into a finished epic instead would hide it completely**,
and leaving it out of an epic makes it visible within one call. Do not create the epic
yourself: a label a new account has never seen cannot be attached, and inventing vocabulary
is planning work.

It lands in `Backlog`, which is right: whether it is dispatchable is not yours to
decide. Say what you opened and where, and leave it there.

Two things about that. **Creating a task spends its number for ever** — deleting it
does not give the number back, which is what keeps `MOLE-19` equal to `#19`, so
nothing throwaway gets created on this board. And **a label you have never seen in
use cannot be attached**, with a bare `403` as the only explanation; use the ones
that are already on other tasks and leave inventing vocabulary to planning.

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

**There is a second label of that shape, and it means a machine that does not exist
yet: `needs-windows`.** A task carries it when the code can be written and reviewed
here but the answer it needs can only be measured on Windows — whether a media
pipeline can be built on a pool thread, whether a manifest really lifts the
260-character ceiling, whether a terminal panel over ConPTY works at all. Planning
keeps those in `Backlog` rather than `Ready`, so you will not be handed one; the
machine itself is `MOLE-253`. **What this asks of you is the other direction**: when
you open a task whose *done when* cannot be reached without a Windows machine, say so
in the message and let planning apply the label, rather than writing a "done when"
that quietly cannot be met. Splitting the assertable half from the measurement is
better still, and three of the tasks in *Windows and macOS, built and run* are
written exactly that way.

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

- [CHANGELOG.md](CHANGELOG.md) — **one line per change, not one sentence.** Newest
  first, and in this shape:

  ```
  2026-08-10 #MOLE-22 Fix something somewhere
  ```

  The day it landed, the ticket, and the shortest phrase that says what changed. No
  design discussion, no internals, no restating the ADR — **that is what the commit
  message and the ADRs are for**, and the commit message is where it goes when the
  first answer turned out to be wrong. Write `#MOLE-22`, never `#22`: GitHub turns `#22` into a link to
  an issue that was deleted on 2026-08-10.

  **This file is the release notes, and its shape is enforced rather than
  conventional.** `tests/scripts/tst_Changelog.sh` reads the two expressions out of
  the file's own header and holds every line against them, so a line in another shape
  fails the suite rather than quietly not reaching anybody. **The header is where the
  rules live — read it rather than this page when you are unsure**, because the whole
  point of them living there is that there is no second copy to disagree with the
  first. The one most likely to catch you out: **every `##` line in the file is a
  release marker**, so nothing else may be a second-level heading. `make release`
  writes the first marker and there is none yet, because nothing has been released;
  until there is, everything in the file is unreleased. Lines from before 2026-08-10
  are prose at the end and stay that way; they belong to the first release.
  [ADR-0080](docs/adr/0080-the-changelog-is-a-structured-log-and-the-release-notes-come-out-of-it.md)
  records the format and what was considered instead.
- [TODO.md](TODO.md) — context that is *not* a task: behaviour we have decided to
  live with, conventions to follow, gaps that are documented rather than
  scheduled.

**There is no `DONE.md` any more, and do not start one.** It held a long account of
every finished task, and at 6,872 lines it had become 43% of the repository's prose —
a second telling of what the commit message already says, since a commit body here
averages 37 lines and covers the diagnosis, the alternatives and the test. The record
now lives in four places and none of them is a file that grows without bound: the
**ticket** for what was asked, the **commit message** for what the answer turned out to
be *including where the first answer was wrong*, an **ADR** for a decision about shape,
and `CHANGELOG.md` for one line per user-visible change. `MOLE-276` deleted it on
2026-08-21 and [ADR-0071](docs/adr/0071-the-record-of-finished-work-is-the-commit.md)
records why. Old entries stay readable at any commit that had them —
`git show <sha>:DONE.md`.

## Which task to take next

**The work is taken epic by epic.** The queue is the `To-Do` column of the `Epics`
board: **the topmost card there is the epic being worked on**, and you stay in it
until it has nothing left in `Ready`. Within an epic, the tasks are taken in the
order they sit in `Ready`. There is no choosing to do at either level.

Two rules, because they answer different questions — the Epics board says *which
topic*, and `Ready` says *which of its tasks*. Skip an epic with nothing in `Ready`:
whatever it has left is in `Backlog` or `Blocked`, and neither is dispatchable.

The Mole board's five columns say what state a task is in:

| Column | Holds |
|---|---|
| `Backlog` | not planned, or the brief is not complete enough to work from |
| `Ready` | planned and dispatchable |
| `In Progress` | being worked on now — one at a time |
| `Blocked` | waiting on something outside the repository, usually the live test environment |
| `Done` | landed and verified |

`Backlog` and `Blocked` are not a queue to dip into. A task leaves them in a
planning session, not because it looked convenient on the day.

Everything below takes the address and the token from the role's own file, never from
anything in this repository:

```sh
set -a; . ~/dev/workspace/mole-pm/environment/vikunja/engineer.env; set +a
v() { curl -sS -H "Authorization: Bearer $VIKUNJA_TOKEN" \
        -H 'Content-Type: application/json' "${@:2}" "$VIKUNJA_URL/api/v2$1"; }

root=$(v /projects  | jq -r '.items[]|select(.title=="Mole")|.id')
epics=$(v /projects | jq -r '.items[]|select(.title=="Epics")|.id')
view=$(v "/projects/$root/views" | jq -r '.items[]|select(.title=="Kanban")|.id')
queue=$(v "/projects/$epics/views" | jq -r '.items[]|select(.view_kind=="kanban")|.id')

# which epic — the top card in To-Do
v "/projects/$epics/views/$queue/buckets/tasks" \
  | jq -r '.items[]|select(.title=="To-Do")|.tasks|sort_by(.position)[]|.title'

# which of its tasks — Ready, in order, with the epic each one belongs to
v "/projects/$root/views/$view/buckets/tasks" \
  | jq -r '.items[]|select(.title=="Ready")|.tasks|sort_by(.position)[]
           |[.identifier, ([.labels[]?|select(.title|startswith("epic: "))|.title]|first),
             .title]|@tsv'
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

**When an epic has nothing left outside `Done`, move its card to `Done` on the Epics
board** — the same way you move your own, and for the same reason: it is the signal
that the next card down is now the work, and nobody else is watching for it.

```sh
epic=$(v "/projects/$epics/tasks?per_page=50" \
       | jq -r '.items[]|select(.title=="Duplicates, rebuilt")|.id')
# a related task carries `index` but an empty `identifier`, so build the name
v "/tasks/$epic" | jq -r '[.related_tasks.subtask[]?|select(.done|not)|"MOLE-\(.index)"]'
```

An empty list there is the whole test. If it is not empty, the epic is still the
work, even when everything left in it sits in `Backlog` or `Blocked` — in that case
say so rather than moving the card, because what happens next is a planning call.

Both orders are deliberate rather than obvious. They are planned separately,
weighing what unblocks the most against what is losing data today, and the
reasoning does not fit on a card. If the top of either queue looks wrong from
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
