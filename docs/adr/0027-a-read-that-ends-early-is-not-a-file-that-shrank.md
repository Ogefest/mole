# ADR-0027: A read that ends early is not a file that shrank

- **Date:** 2026-08-10
- **Status:** Accepted. Extends [ADR-0016](0016-a-copy-is-weighed-at-the-destination.md),
  which weighs the other end.

## Context

A copy is planned from a listing, and the plan carries a size for every file.
When the copy delivers fewer bytes than that, `TransferTask` used to write a
warning to the log and call the transfer a success.

Two quite different things produce that discrepancy, and from inside the copy
loop they are identical. The file may really have got smaller between the listing
and the copy — somebody truncated it, a log was rotated — in which case what
arrived is the file as it now is, and there is nothing wrong. Or the stream ended
early and said it was the end of the file, which is what a dropped connection
looks like through `QIODevice`: `read()` returns zero, exactly as it does at the
end.

The second one is the expensive mistake. A copy that stopped at 30% is reported
as a copy; the destination holds a file that looks complete, because it is the
size it is; and a *move* then deletes the source, which was the only whole copy.
The check added in ADR-0016 does not catch it — it weighs what arrived against
what was sent, and both of those are 30%.

## Decision

**When a copy delivers fewer bytes than the plan said, the source is asked
again.** If it now reports exactly what was copied, the file shrank and the copy
stands. If it reports anything else — the larger size still, or an error — the
read ended early, and the transfer of that file fails with `the source said N
bytes and gave M`.

**The question is asked before the destination is closed**, because closing is
what puts a write in place. A copy that is about to be called a failure must not
first be renamed into the name somebody asked for.

**One extra `stat`, and only when there is a discrepancy to explain.** A copy
that delivers what was expected — every copy, nearly always — costs nothing.

**A file that grew is still only a warning.** Everything it had when it was
opened arrived, which is all a copy can promise.

## Reason

**Why ask the source rather than trust the plan.** The plan is a claim from a
listing that may be minutes old, and failing every copy that disagrees with it
would fail the ordinary case of a file that legitimately changed. The source is
the only party that can say which happened, and it is one call away.

**Why not compare against the destination instead.** That is ADR-0016, and it
already runs. It cannot answer this question: the destination faithfully holds
what was sent, so a short read passes it.

**Why not read the source twice and compare.** For a large file over a network
that doubles the cost of every copy to catch a case that is rare — and it still
would not distinguish a file that shrank between the two reads.

**Why fail rather than retry.** A retry is worth having and is not this decision.
Failing puts the file in the failure list with the reason, leaves the source
alone, and stops a move from deleting anything; retrying can be built on top of
that without changing what the guard decides.

## Consequences

- A backend whose listing overstates sizes will now fail copies rather than
  producing short files silently. That is the intended reading: a drive that
  cannot say how big a file is cannot be trusted to say that all of it arrived.
- The failure names the file, what was claimed and what arrived, so the log says
  which of the two cases it was without anyone having to reproduce it.
- A move whose copy was cut short keeps its source, since a move deletes nothing
  while anything in the transfer failed.
- Sync has its own copy loop and does not yet ask this question. It is recorded
  in TODO.md.
