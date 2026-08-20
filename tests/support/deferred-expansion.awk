# Finds a `\$` on a line that runs in the outer shell rather than on a machine.
#
# The general form of the MOLE-233 fault. A testbed script is two programs in one
# file: the lines that run where it is invoked, and the heredocs it ships to a
# server. Inside a heredoc, `\$` is how expansion is deferred to that server and
# is correct. Outside one it is almost always a mistake -- and the mistake that
# happened wrote `[ -z "\$NFS_CLIENTS" ]` in the outer script, so the test
# compared a literal string that is never empty, the guard never held, and an NFS
# share stood exported read-write to a whole network for nine days.
#
# In its own file rather than inline in the test, because a checker that looks for
# `\$` and is written with `\$` in it reports itself. The pattern comes in through
# `-v backslash_dollar=…` for the same reason.
#
# Prints `file:line:text` for each offender and nothing at all when clean.

# Heredoc depth is per file, and losing count is reported rather than swallowed.
#
# This is the one way a check like this fails silently: a heredoc whose terminator
# the tracker does not recognise leaves depth above zero for ever, and then every
# line of every file after it is skipped as though it were heredoc body. The check
# goes green by looking at nothing. It happened while this was being written --
# `$((1 << SHIFT))` reads as an opening heredoc -- so the count is now announced.
FNR == 1 {
    if (depth > 0)
        printf "%s:%d:unterminated heredoc <<%s -- this checker lost count here\n",
               prevfile, prevline, delim[depth]
    depth = 0
}

{ prevfile = FILENAME; prevline = FNR }

# Inside a heredoc: nothing to check, only a terminator to watch for. Bash accepts
# leading whitespace on a `<<-` terminator, so it is trimmed either way; a `<<`
# terminator with whitespace would be a syntax error the parse check catches first.
depth > 0 {
    line = $0
    gsub(/^[ \t]+|[ \t]+$/, "", line)
    if (line == delim[depth]) depth--
    next
}

# An outer line. A comment is prose about the fault rather than the fault.
$0 ~ backslash_dollar && $0 !~ /^[ \t]*#/ { printf "%s:%d:%s\n", FILENAME, FNR, $0 }

# Then note every heredoc this line opens, in order, so two on one line are both
# tracked. A `<<` that is really a left shift is excluded by requiring the tag to
# start with a letter or underscore *and* not be followed by more expression --
# imperfect, which is why losing count is reported above rather than assumed away.
{
    rest = $0
    # A here-string is not a heredoc. `<<<"hello"` used to read as `<` followed by
    # `<<"hello"`, which opened a heredoc that never closed and blinded this
    # checker for every file after check-services.sh.
    gsub(/<<</, " ", rest)
    while (match(rest, /<<-?[ \t]*[\047"]?[A-Za-z_][A-Za-z0-9_]*[\047"]?/)) {
        tag = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        sub(/^<<-?[ \t]*[\047"]?/, "", tag)
        sub(/[\047"]$/, "", tag)
        depth++
        delim[depth] = tag
    }
}

END {
    if (depth > 0)
        printf "%s:%d:unterminated heredoc <<%s -- this checker lost count here\n",
               prevfile, prevline, delim[depth]
}
