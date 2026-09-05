#!/usr/bin/env bash
#
# What `services.sh` sends to a machine, asserted without a machine.
#
# The fault this exists for: the guard deciding whether to export the NFS share
# was written `[ -z "\$NFS_CLIENTS" ]` -- with a backslash belonging to a heredoc
# four lines further down. The line runs in the outer script, so the test compared
# the literal string `$NFS_CLIENTS`, which is never empty. The guard never held,
# the `else` branch ran every time with an empty client list, and `exportfs` reads
# a missing host as `*`. Every machine this script provisioned had its NFS share
# exported read-write to every host on its network, for nine days, while the
# comment above the variable said the export must never be "the whole subnet by
# accident".
#
# The skip branch the author wrote as the safe default was dead code that had
# never printed once, and nothing in this repository could have noticed: `tests/`
# was C++ only and no shellcheck ran anywhere.
#
# So the general form is what is asserted, not just the one line: an export whose
# host field is empty must never reach a machine, whatever the guard does.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

# Invented, and deliberately not the address of anything. The stub `ssh` goes
# nowhere, so these only have to be non-empty -- and a real address in a test
# fixture is exactly what CLAUDE.md forbids.
export MOLE_TESTBED_ADDRESS=testbed.invalid
export MOLE_TESTBED_PASSWORD=throwaway-not-a-real-password
export MOLE_TESTBED_ACCOUNT=moletest

# The export path the script owns, and the signature of the fault: the path,
# whitespace, then straight into the option list with no host in between.
EXPORT_PATH=/srv/moledata/nfs
EMPTY_HOST="^$EXPORT_PATH[[:space:]]*\("

# --- the guard holds ---------------------------------------------------------

begin "with no client named, no export reaches the machine and the skip is announced"
stub_ssh
unset MOLE_TESTBED_NFS_CLIENTS
run_script scripts/testbed/services.sh
exited 0
# The whole point: nothing that reaches the machine writes an export line.
never_reached "$EXPORT_PATH ("
never_reached_matching "$EMPTY_HOST"
never_reached_matching "^$EXPORT_PATH.*\(rw,"
# And the branch that was dead code for nine days now prints.
said "skipped: set MOLE_TESTBED_NFS_CLIENTS"
said "An export open to the whole LAN is not something to arrive at by default."
# A machine provisioned while the guard was broken is put right by running this
# again, so the skip has to withdraw a stale export rather than ignore it.
reached "exportfs -ra"
reached_matching ': > /etc/exports'

# --- the guard lets a named client through -----------------------------------

begin "with a client named, the export names that client and only that client"
stub_ssh
export MOLE_TESTBED_NFS_CLIENTS=203.0.113.7
run_script scripts/testbed/services.sh
exited 0
reached "$EXPORT_PATH 203.0.113.7(rw,sync,insecure,no_subtree_check)"
# Still no empty host, and no wildcard: `exportfs` reads both as the whole LAN.
never_reached_matching "$EMPTY_HOST"
never_reached_matching "^$EXPORT_PATH[[:space:]]+\*\("
# no_root_squash is not an option this export may ever carry: the share is a
# place to put files, not a way to become root on the machine. Asserted against
# the export line rather than the whole transcript -- the script has a comment
# saying it is deliberately not set, and a test that cannot tell a comment from
# an option is a test that goes red for the wrong reason.
never_reached_matching "^$EXPORT_PATH.*no_root_squash"
said "203.0.113.7"

# --- a client list, still no wildcard ----------------------------------------

begin "several named clients are still several named clients"
stub_ssh
export MOLE_TESTBED_NFS_CLIENTS=203.0.113.7
run_script scripts/testbed/services.sh
exited 0
never_reached_matching "$EMPTY_HOST"
never_reached_matching "[[:space:]]\*\(rw"

# --- what runs on the machine has to arrive as something that can run --------

begin "the checks that ask the machine about itself reach it as commands, not as answers"
# The MOLE-354 fault, from the only angle a test without a machine can see it.
#
# `grep -q P <<<"$(producer)"` inside an unquoted heredoc is evaluated here, once,
# while the heredoc is being built -- so what reaches the machine is the
# workstation's answer as a literal, and the command never runs there at all.
# Three of them went that way: a wait for vsftpd to let go of port 21 asked this
# machine's socket table, the MinIO versioning check ran `mc` where there is no
# `mc`, and a snapshot script listed the wrong host's snapshots.
#
# What makes it visible from here is that the transcript is what was *sent*: a
# substitution that happened locally leaves no command in it. So these two
# assertions are the shape of the fault, and neither of them holds on the tree as
# it was.
stub_ssh
export MOLE_TESTBED_NFS_CLIENTS=203.0.113.7
run_script scripts/testbed/services.sh
exited 0
reached "ss -ltn"
reached "mc version info"
# And the escape really is an escape rather than a literal backslash arriving on
# the far side, which would be a syntax error there.
#
# Spelled in bracket expressions because a backslash followed by a dollar in this
# file is a finding in its own right -- tst_ShellScripts reads these lines too,
# and it is right to: this line runs here. `[\\]` is a literal backslash to an
# ERE and is not that sequence.
never_reached_matching '[\\][$][(]ss -ltn'
never_reached_matching '[\\][$][(]/usr/local/bin/mc version info'

# --- the account that only a key gets into -----------------------------------

# A made-up public key. It never authenticates anything: what is being checked is
# that the bytes handed in are the bytes that reach the machine, and a real key
# here would be a credential in a public repository for no gain at all.
FAKE_KEY="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFakeKeyForTheTestSuiteOnly000000000000 mole-test"

begin "with no public key given, no key-only account reaches the machine and the skip is announced"
stub_ssh
unset MOLE_TESTBED_SFTP_KEY_PUB
run_script scripts/testbed/services.sh
exited 0
# The account is not half-made: nothing about it is sent at all.
never_reached "useradd -m -s /bin/bash molekey"
never_reached "Match User molekey"
never_reached_matching "authorized_keys"
said "MOLE_TESTBED_SFTP_KEY_PUB is not set"
# And it says what the silence costs, because a suite that skips looks like a
# suite that passed. See MOLE-423.
said "skips without it, which is a result rather than a pass"

begin "given a public key, the account is made, carries that key, and is told to refuse passwords"
stub_ssh
export MOLE_TESTBED_SFTP_KEY_PUB="$FAKE_KEY"
run_script scripts/testbed/services.sh
exited 0
reached "useradd -m -s /bin/bash molekey"
# The key that was handed in, not one the script invented.
reached "$FAKE_KEY"
reached "/home/molekey/.ssh/authorized_keys"
# Somewhere of its own, so a passing case cannot be one that wrote into the
# password account's directory.
reached "/home/molekey/sftp"
# The refusal, and scoped to this account: an unscoped PasswordAuthentication no
# locks every other live suite out of the machine, and there is no getting back
# in to undo it.
#
# **Asserted as one fragment rather than as two lines.** The whole block reaches
# the machine inside a single printf, so the transcript holds it as one line with
# literal backslash-n between the parts -- which means an anchored expression
# matches nothing whatever the script says, and `reached "Match User molekey"`
# passes on the strength of the grep two lines above it. Both of those were
# written here first and both were green against a version that had moved the
# refusal to the top level. The fragment below is the shape that is not: the
# keyword indented under its Match, in that order, in those bytes.
reached 'Match User molekey\n    PasswordAuthentication no'
# And the version that would do the damage: the keyword at column one, directly
# after a newline escape, belonging to no Match block. Written in a bracket
# expression because a backslash next to a dollar in this file is a finding of
# its own -- see the case above.
never_reached_matching '[\]nPasswordAuthentication'
never_reached_matching '[\]nKbdInteractiveAuthentication'
# The account every other suite uses is not touched by any of it.
never_reached "userdel moletest"
never_reached "Match User moletest"
# A broken sshd config locks the machine out for good, so the config is tested
# before the daemon is restarted.
reached_before "sshd -t -f /etc/ssh/sshd_config" "systemctl restart ssh"
# And the private half is nowhere near any of this.
never_reached_matching "BEGIN [A-Z ]*PRIVATE KEY"
unset MOLE_TESTBED_SFTP_KEY_PUB

# --- and it refuses to run on nothing ----------------------------------------

begin "without an address the script provisions nothing and says why"
stub_ssh
unset MOLE_TESTBED_ADDRESS
run_script scripts/testbed/services.sh
exited 1
said "Set MOLE_TESTBED_ADDRESS"
# Nothing at all was sent: a script that refuses must refuse before it acts.
if [ -s "$TRANSCRIPT" ]; then fail "it reached a machine after refusing to run"; fi
export MOLE_TESTBED_ADDRESS=testbed.invalid

done_testing
