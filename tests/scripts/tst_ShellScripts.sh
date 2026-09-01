#!/usr/bin/env bash
#
# What every script in `scripts/` must be true of, checked statically.
#
# Companion to tst_TestbedProvisioning.sh, which runs one script and asserts on
# what it sent. This one runs none of them: it holds the rules that can be
# checked by reading, over all of them at once, so a new script joins the suite
# by existing rather than by somebody remembering to add it.
#
# The rule that matters is the general form of the MOLE-233 fault. A testbed
# script is two programs in one file -- the lines that run here, and the heredocs
# that run on the machine -- and `\$` is how a line in the second kind defers
# expansion to the server. On a line of the first kind it is almost always a
# mistake, and the one that happened compared the literal string `$NFS_CLIENTS`
# against empty, so a guard never held and an NFS share was exported to a whole
# network for nine days.
#
# `shellcheck` flags this class and is the right long-term answer; it is not
# installed on this machine and is not a build dependency, so the rule is held
# here instead of assumed. See TODO.md.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

cd "$MOLE_SOURCE_DIR" || exit 1

scripts=$(find scripts tests -name '*.sh' | sort)

begin "there are scripts to check at all"
# A find that matched nothing would make every case below vacuously green, which
# is the failure mode of every test that iterates over a glob.
count=$(printf '%s\n' "$scripts" | grep -c .)
[ "$count" -ge 8 ] || fail "found only $count shell scripts; expected the testbed set"

begin "every script parses"
for f in $scripts; do
    bash -n "$f" 2>"$SHELLTEST_TMP/syntax" || {
        fail "$f does not parse"
        sed 's/^/    /' "$SHELLTEST_TMP/syntax"
    }
done

begin "every script refuses to read an unset variable as empty"
# `set -u` is what turns a renamed variable into an error instead of an empty
# string, and an empty string is what `exportfs` reads as "every host".
#
# Only the scripts under `scripts/`: the tests get theirs from shelltest.sh, which
# they source on their first line, so asking each of them to repeat it would be
# asking for a line that means nothing.
for f in $scripts; do
    case "$f" in tests/*) continue ;; esac
    grep -qE '^set -[a-z]*u' "$f" || fail "$f does not set -u"
done

begin "every shell test gets its rules from the support file"
for f in $scripts; do
    case "$f" in tests/scripts/*) ;; *) continue ;; esac
    grep -q 'support/shelltest.sh' "$f" || fail "$f does not source shelltest.sh"
done

begin "no line that runs locally defers expansion to a machine"
# The MOLE-233 class, over every script at once. The checker lives in
# tests/support/deferred-expansion.awk -- see the note at the top of it for why it
# cannot be written inline here.
#
# The pattern is passed in rather than written into the program for the same
# reason: an awk source file containing a literal backslash-dollar would match
# itself the moment anybody pointed this check at the support directory.
awk -v backslash_dollar='\\\\[$]' \
    -f "$MOLE_SOURCE_DIR/tests/support/deferred-expansion.awk" \
    $scripts > "$SHELLTEST_TMP/deferred"
if [ -s "$SHELLTEST_TMP/deferred" ]; then
    fail "deferred expansion outside a heredoc, or a heredoc the checker could not close -- see MOLE-233"
    sed 's/^/    /' "$SHELLTEST_TMP/deferred"
fi

begin "no function takes the name of a program the script would otherwise run"
# A function named after a command on the machine replaces that command for the
# rest of the file, silently. A test script defined one called `cut`, and a
# `grep -n ... | cut -d: -f1` a few lines later ran the function instead of the
# program -- an hour at the wrong end of a pipe, on a fault that reading the file
# would have caught. See MOLE-294.
#
# **What counts as a program is what the machine has, plus a floor that does not
# depend on it.** Keyed only to the live PATH, the rule would pass or fail by what
# happens to be installed where it runs, which is a check nobody can trust; keyed
# only to a written list, it goes stale. The floor is the utilities these scripts
# actually use, so the case means the same thing on a machine with a thin PATH --
# a container, or a hook that runs with almost nothing set.
#
# **Shell builtins are deliberately not flagged.** Overriding `echo` or `cd`
# inside a test is a legitimate thing to do, and a few of the names below exist on
# disk as well as in the shell -- `printf` and `test` among them. What is being
# looked for is a function standing in front of a program the shell would
# otherwise have run.
floor="awk basename cat chmod chown cmp cp curl cut date dd df diff dirname du env expr file find
       grep gzip head id install ln ls make mkdir mktemp mv od printf ps readlink realpath rm rmdir
       sed seq sort stat tail tar tee test touch tr uname uniq wc which xargs xz zip"

is_a_program() { # 0 when a program of this name is what the shell would run
    case " $floor " in *" $1 "*) return 0 ;; esac
    [ -n "$(type -P -- "$1" 2>/dev/null)" ]
}
is_a_builtin() { [ "$(type -t -- "$1" 2>/dev/null)" = builtin ]; }

: > "$SHELLTEST_TMP/shadowed"
for f in $scripts; do
    grep -nE '^[[:space:]]*(function[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(\)' "$f" \
        > "$SHELLTEST_TMP/definitions" || continue
    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        at=${hit%%:*}
        name=$(printf '%s' "${hit#*:}" \
            | sed -E 's/^[[:space:]]*(function[[:space:]]+)?([A-Za-z_][A-Za-z0-9_]*).*/\2/')
        is_a_builtin "$name" && continue
        is_a_program "$name" && printf '%s:%s: %s\n' "$f" "$at" "$name" >> "$SHELLTEST_TMP/shadowed"
    done < "$SHELLTEST_TMP/definitions"
done
if [ -s "$SHELLTEST_TMP/shadowed" ]; then
    fail "a function stands in front of a program, so every later use of that name in the file runs the function instead -- rename it"
    sed 's/^/    /' "$SHELLTEST_TMP/shadowed"
fi

begin "the floor still answers on a machine with nothing on PATH"
# The half of the rule that would rot in silence: with an empty PATH every name
# looks like it is not a program, and the case above would pass over a script that
# shadows every one of them. Nothing external is called while PATH is empty --
# `type` and `case` are the shell's own.
saved_path=$PATH
PATH=''
searched=$(type -P -- cut 2>/dev/null)
answered=no
is_a_program cut && answered=yes
PATH=$saved_path
[ -z "$searched" ] || fail "PATH was not actually empty, so this proved nothing"
[ "$answered" = yes ] || fail "with nothing on PATH the rule stopped recognising cut as a program"

begin "nothing in a script names a real machine"
# CLAUDE.md's rule, held rather than trusted: every address these scripts use
# comes from the environment at run time. A literal address in a tracked file is
# the signal that the code wanted a parameter and got a constant.
#
# Reserved documentation ranges are allowed, because a test fixture has to name
# something: 192.0.2.0/24, 198.51.100.0/24 and 203.0.113.0/24 (RFC 5737).
grep -nHE '(^|[^0-9.])(10|172|192)\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}' $scripts \
    | grep -vE '192\.0\.2\.' > "$SHELLTEST_TMP/addresses"
if [ -s "$SHELLTEST_TMP/addresses" ]; then
    fail "a private address is written into a tracked script"
    sed 's/^/    /' "$SHELLTEST_TMP/addresses"
fi


begin "nothing pipes a program's output into grep -q"
# **`producer | grep -q pattern` fails at random wherever `pipefail` is set**, which
# is every script here. `grep -q` exits the moment it matches; the producer's next
# write lands on a closed pipe, takes SIGPIPE, and pipefail reports the pipeline as
# failed although the pattern was found. Whether it happens is a matter of how the
# two processes are scheduled, so it passes on an idle machine and fails under load.
#
# Measured: `sed -n '/is_excluded()/,/^}/p' scripts/make-bundle.sh | grep -q Apache-2.0`
# failed twice in two hundred runs on an idle workstation, and once on a CI runner
# that was also running the rest of the tier. Two of the sites were inside
# `set -euo pipefail`, where a spurious failure does not report anything -- it kills
# the script; one of those decided whether a bundle carries the multimedia plugin
# tree, and one is in the licence check a release runs over every artefact.
#
# The fix is a here-string rather than a pipe -- `grep -q P <<<"$(producer)"` -- which
# keeps the same grep and cannot lose the race. See MOLE-329.
#
# **`printf … | grep -q` is deliberately allowed**, and there are about twenty of
# them. `printf` writes once and exits, so its write always completes before grep can
# close the pipe. What is refused is a producer that writes more than one buffer,
# which is every program.
: > "$SHELLTEST_TMP/racy-pipes"
for f in $scripts; do
    # A real pipe, not `||`: one bar with no bar on either side of it. And the
    # left-hand side asked about, because that is what decides whether it can race.
    awk -v file="$f" '
        # Prose about the shape is not the shape. This file explains it at length,
        # and the MOLE-233 rule above had to learn the same lesson about its own
        # comments.
        /^[ \t]*#/ { next }
        /\|\|/ { next }
        /[^|]\|[ \t]*grep[ \t]+-[a-zA-Z]*q/ {
            before = $0
            sub(/\|[ \t]*grep.*/, "", before)
            if (before ~ /printf|echo/) next
            printf "%s:%d:%s\n", file, FNR, $0
        }
    ' "$f" >> "$SHELLTEST_TMP/racy-pipes"
done
if [ -s "$SHELLTEST_TMP/racy-pipes" ]; then
    # Single-quoted, so the example needs no backslash before its dollar -- which
    # the rule above would have refused, correctly, as deferred expansion on a line
    # that runs here.
    fail 'a pipeline into grep -q reports failure at random under pipefail; put the producer in a here-string instead'
    sed 's/^/    /' "$SHELLTEST_TMP/racy-pipes"
fi

done_testing
