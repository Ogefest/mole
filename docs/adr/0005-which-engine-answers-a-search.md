# ADR-0005: The search form decides whether to walk or to ask the index

- **Date:** 2026-08-08
- **Status:** Accepted

## Context

There were two searches and no relationship between them. `Ctrl+F` opened a tab
that walked a tree — correct, current, and slow over a large disk. `Ctrl+Shift+I`
opened a different tab that queried the index — instant, possibly stale, and over
whole volumes rather than the folder you were looking at. A person searching a
folder that had already been indexed got the slow answer unless they knew the
other tab existed and knew that their folder was inside an indexed volume.

## Decision

The `Ctrl+F` form decides, and says what it decided.

It asks the index when both hold:

- an indexed volume's root is a prefix of the folder being searched, so the index
  covers the whole subtree in question, and
- the *Use the index* toggle is on. It is on by default and lives with the other
  criteria.

Otherwise it walks. Partial coverage counts as no coverage.

The status line always names the engine that answered, and for the index it also
says when that volume was last scanned. The toggle can be turned off for the case
that matters: the truth on disk right now, whatever the index remembers.

The index-search tab stays as it is. It answers a different question — *find this
across every volume I have indexed* — and folding it into the folder search would
lose that.

## Reason

Partial coverage was the interesting case, and the temptation was to serve it: ask
the index for the part it covers and walk the rest. Rejected, because the result
would be one list of which some rows are current and some are as old as the last
scan, with nothing on the row to say which — an answer nobody can reason about. A
search either has one freshness or it should not pretend to be one search.

Choosing per search rather than as a setting follows from what the two engines
actually differ in: not speed but *when they were right*. That is a property of the
question being asked ("what is there now" versus "what did we last see"), not of
the person asking, so it belongs next to the query and not in a preferences page.

The toggle is on by default because the index is enormously faster and, for a
folder that was indexed, usually right. Saying which engine answered is what makes
that default safe: a wrong answer that admits where it came from is recoverable,
and one that does not is a trap.

## Consequences

- The form has to know about the index, so `LiveSearchController` gained a
  dependency it did not have. It reads volumes from `IndexDatabase` and compares
  roots as string prefixes on the uri, which is the same comparison a mount lookup
  makes.
- An index that has never been scanned covers nothing, so the folder search behaves
  exactly as it did before anyone indexed anything.
- Two engines answering the same form means two paths for every criterion. Size
  filters exist on both (`IndexSearchQuery` and `LiveSearchTask::Criteria` each have
  `minSize`/`maxSize`), which is why size was the criterion added first; anything
  the index cannot express has to fall back to walking, and that has to be said out
  loud in the status line rather than silently ignored.
- The tests cover the choice itself: an unindexed folder walks, an indexed folder
  answers from the index, the toggle forces a walk, and a folder that is only
  partly covered walks.
