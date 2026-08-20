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

done_testing
