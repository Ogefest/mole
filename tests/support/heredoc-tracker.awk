# Tells the two programs a testbed script contains apart, for a rule that has to
# know which one a line belongs to.
#
# A testbed script is one file holding two programs: the lines that run where it
# is invoked, and the heredocs it ships to a server. Almost every static rule
# about these scripts is really a rule about one of the two, and a checker that
# cannot tell them apart gets it wrong in both directions -- MOLE-233 was a line
# that deferred expansion in the outer program, and MOLE-354 was the fix for a
# racy pipe being applied inside a heredoc, where it moved a server-side pipeline
# onto the workstation.
#
# Included ahead of the rule that uses it: awk takes several `-f` files and runs
# their rules in file order against each line, so anything after this can read
#
#   inHeredoc       -- this line is inside a heredoc body, its terminator included
#   heredocQuoted   -- that heredoc was opened <<'TAG', so nothing in it expands
#                      locally and every rule about local expansion is moot
#   heredocTag      -- the terminator being waited for
#
# It deliberately does not `next` on a heredoc line: one of its two users has a
# rule *about* heredoc bodies. Guard with `!inHeredoc` where a rule is about the
# outer program.

# Depth is per file, and losing count is reported rather than swallowed.
#
# This is the one way a check built on this fails silently: a heredoc whose
# terminator the tracker does not recognise leaves the depth above zero for ever,
# every line of every file after it reads as heredoc body, and a rule guarded by
# `!inHeredoc` then looks at nothing and goes green. It happened while the first
# version of this was being written -- `$((1 << SHIFT))` reads as an opening
# heredoc -- so the count is announced.
FNR == 1 {
    if (depth > 0)
        printf "%s:%d:unterminated heredoc <<%s -- this checker lost count here\n",
               prevfile, prevline, delim[depth]
    depth = 0
}

{
    prevfile = FILENAME
    prevline = FNR
    inHeredoc = (depth > 0)
    heredocQuoted = inHeredoc ? quoted[depth] : 0
    heredocTag = inHeredoc ? delim[depth] : ""
}

# The terminator closes the heredoc and is itself part of it. Bash accepts
# leading whitespace on a `<<-` terminator, so it is trimmed either way; a `<<`
# terminator with whitespace would be a syntax error the parse check catches
# first.
inHeredoc {
    trimmed = $0
    gsub(/^[ \t]+|[ \t]+$/, "", trimmed)
    if (trimmed == delim[depth])
        depth--
}

# Every heredoc an outer line opens, in order, so two on one line are both
# tracked. A `<<` that is really a left shift is excluded by requiring the tag to
# start with a letter or underscore -- imperfect, which is why losing count is
# reported above rather than assumed away.
!inHeredoc {
    rest = $0
    # A here-string is not a heredoc. `<<<"hello"` used to read as `<` followed
    # by `<<"hello"`, which opened a heredoc that never closed and blinded the
    # first version of this for every file after check-services.sh.
    gsub(/<<</, " ", rest)
    while (match(rest, /<<-?[ \t]*[\047"]?[A-Za-z_][A-Za-z0-9_]*[\047"]?/)) {
        tag = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        sub(/^<<-?[ \t]*/, "", tag)
        # Quoted means nothing in the body expands here at all, which is what a
        # rule about local expansion has to know before it says anything.
        wasQuoted = (tag ~ /^[\047"]/)
        sub(/^[\047"]/, "", tag)
        sub(/[\047"]$/, "", tag)
        depth++
        delim[depth] = tag
        quoted[depth] = wasQuoted
    }
}

END {
    if (depth > 0)
        printf "%s:%d:unterminated heredoc <<%s -- this checker lost count here\n",
               prevfile, prevline, delim[depth]
}
