# Two rules about where a `$` is evaluated, one for each of the two programs a
# testbed script contains. Run after heredoc-tracker.awk, which says which
# program a line belongs to.
#
# **Outside a heredoc, `\$` is a mistake.** The general form of MOLE-233: `\$` is
# how expansion is deferred to a server, and on a line that runs where the script
# was invoked it defers nothing. The mistake that happened wrote
# `[ -z "\$NFS_CLIENTS" ]` in the outer script, so the test compared a literal
# string that is never empty, the guard never held, and an NFS share stood
# exported read-write to a whole network for nine days.
#
# **Inside an unquoted heredoc, an unescaped `$(` is a mistake.** The same fault
# from the other side, and how MOLE-354 happened: the fix for a racy pipe
# rewrote `producer | grep -q P` into `grep -q P <<<"$(producer)"` in three
# heredoc bodies, and bash runs that substitution once, locally, while it is
# building the heredoc. So a loop waiting for a port asked the workstation's
# socket table, a MinIO check ran `mc` on a machine that does not have it, and a
# snapshot script listed the workstation's snapshots. All three were sent to the
# server as literal text that could not do what it said. A plain `$VAR` in such a
# heredoc is the ordinary case and is not a finding -- filling those in locally
# is what leaving the heredoc unquoted is *for*. Running a program is not.
#
# In its own file rather than inline in the test, because a checker that looks
# for `\$` and is written with `\$` in it reports itself. The pattern comes in
# through `-v backslash_dollar=…` for the same reason.
#
# Prints `file:line:text` for each offender and nothing at all when clean.

# An outer comment is prose about the fault rather than the fault. The file above
# this and the test that runs it have both had to learn that about their own
# explanations. A comment *inside* a heredoc is not exempt -- see above.
!inHeredoc && /^[ \t]*#/ { next }

!inHeredoc && $0 ~ backslash_dollar { printf "%s:%d:%s\n", FILENAME, FNR, $0 }

# A substitution that would run here, in a heredoc bound for somewhere else. A
# quoted heredoc expands nothing locally, so there is nothing to get wrong in one.
inHeredoc && !heredocQuoted {
    # What is left once every properly escaped one is taken out. Written this way
    # round because a line may hold both: the MinIO check has a deferred
    # substitution and a local $S3_VERSIONED_BUCKET on it, and only one of the
    # two is wrong. \140 is the backtick, spelled that way so this file can be
    # read by the very rule it implements.
    rest = $0
    gsub(/\\[$][(]/, "", rest)
    gsub(/\\[\140]/, "", rest)
    if (rest ~ /[$][(]/ || rest ~ /[\140]/)
        printf "%s:%d:%s\n", FILENAME, FNR, $0
}
