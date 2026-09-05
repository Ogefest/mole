# Finds `producer | grep -q pattern` on a line that runs under `pipefail`. Run
# after heredoc-tracker.awk, which is what keeps it out of the heredocs.
#
# **The pipeline fails at random wherever `pipefail` is set**, which is every
# script here. `grep -q` exits the moment it matches; the producer's next write
# lands on a closed pipe, takes SIGPIPE, and pipefail reports the pipeline as
# failed although the pattern was found. Whether it happens is a matter of how
# the two processes are scheduled, so it passes on an idle machine and fails
# under load. The fix is a here-string -- `grep -q P <<<"$(producer)"` -- which
# keeps the same grep and cannot lose the race. See MOLE-329.
#
# **Not inside a heredoc**, and that is the whole reason this moved out of the
# test into a file with the tracker in front of it. A heredoc body runs on a
# server, under whatever options *it* sets, and none of these set pipefail there;
# rewriting one of those pipes into a here-string moves the producer onto the
# workstation, which is how a wait for a port came to ask the wrong machine and a
# provisioning script could no longer finish. See MOLE-354. A heredoc that does
# set pipefail is a different matter, and is checked below.
#
# **`printf … | grep -q` is refused as well, and it used to be allowed.** The
# exemption said "printf writes once and exits, so its write always completes
# before grep can close the pipe", and that is true only while the payload fits
# in the pipe buffer. Measured on 2026-09-05, twice and independently: 30 of 30
# pipelines report failure with a 200,000-line payload, 0 of 30 with three lines,
# against a 4 KiB buffer. A rule whose safety depends on how big a variable
# happens to be is one that goes wrong silently the first time a file list grows,
# and nothing would be watching -- `tst_Bundle` failed twice under load on a
# `printf` of a launcher script before this was taken out. A here-string is the
# same grep and cannot lose the race. See MOLE-417.

# Prose about the shape is not the shape.
/^[ \t]*#/ { next }
# `||` is not a pipe.
/\|\|/ { next }

# A heredoc that turns pipefail on for the far side brings the fault with it, so
# its body is checked like an outer line. None does today; the alternative is a
# rule that silently stops applying the moment one is written. The order of these
# three matters: the flag has to be noticed before the skip that reads it.
!inHeredoc { remoteHasPipefail = 0 }
inHeredoc && /^[ \t]*set[ \t].*pipefail/ { remoteHasPipefail = 1 }
inHeredoc && !remoteHasPipefail { next }

/[^|]\|[ \t]*grep[ \t]+-[a-zA-Z]*q/ {
    printf "%s:%d:%s\n", FILENAME, FNR, $0
}
