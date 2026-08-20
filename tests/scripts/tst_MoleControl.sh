#!/usr/bin/env bash
#
# The sweep that takes away what a killed run left on the test machine, asserted
# against a temp tree instead of a machine.
#
# The fault this exists for: both heavy tiers clean up in `cleanup()`, which works
# for every run that reaches the end of a case. A run that does not -- killed by a
# watchdog, by SIGABRT, by Ctrl-C, or by the machine going away -- leaves its
# payload behind, and nothing ever took it away. Nineteen gigabytes in twenty-five
# files had built up over two days when MOLE-235 was written.
#
# What makes it worse than untidiness: **every case in both tiers declines with a
# reason when the destination has no room.** That is right, and it is what stops a
# suite taking a test machine down with it -- but room eaten by our own abandoned
# payloads is indistinguishable from a machine that is genuinely too small. So the
# tier starts skipping for a reason that is not true, and reports green for having
# done nothing. That is the failure the testing project exists to remove, arriving
# by a different door.
#
# `mole-control` runs on the machine, so `control.sh emit` prints it and these
# cases run it here with MOLE_CONTROL_DATA and MOLE_CONTROL_HOME pointed at a temp
# tree. No ssh, no server, no root.
#
. "$(dirname "${BASH_SOURCE[0]}")/../support/shelltest.sh"

CONTROL="$SHELLTEST_TMP/mole-control"
bash "$MOLE_SOURCE_DIR/scripts/testbed/control.sh" emit > "$CONTROL" || {
    printf 'could not emit mole-control\n'; exit 1
}
chmod +x "$CONTROL"

# The layout services.sh builds, in a temp tree. `sftp` hangs off a home directory
# and everything else off the data disk, which is what the real machine looks like.
TREE="$SHELLTEST_TMP/tree"
export MOLE_CONTROL_DATA="$TREE/moledata"
export MOLE_CONTROL_HOME="$TREE/home"
export MOLE_CONTROL_MINIO="$TREE/minio"

fresh_tree() {
    rm -rf "$TREE"
    mkdir -p "$MOLE_CONTROL_HOME/sftp" "$MOLE_CONTROL_MINIO" \
             "$MOLE_CONTROL_DATA"/{webdav,ftp/Shared,smb,nfs}
}

# Runs the sweep and keeps its output where `said` can see it.
sweep() {
    "$CONTROL" sweep "$@" >"$SCRIPT_OUTPUT" 2>&1
    SCRIPT_STATUS=$?
}

gone()  { [ -e "$1" ] && fail "still there: $1"; return 0; }
still() { [ -e "$1" ] || fail "taken away and should not have been: $1"; return 0; }

# --- the payloads a killed run leaves -----------------------------------------

begin "a killed run's payload is taken away from every service root"
fresh_tree
# One of each name the suites actually create, spread over the roots they use.
: > "$MOLE_CONTROL_HOME/sftp/mole-cut-4001.bin"
: > "$MOLE_CONTROL_HOME/sftp/mole-heavy-4002.bin"
: > "$MOLE_CONTROL_HOME/sftp/mole-upload-4003.bin.mole-partial"
: > "$MOLE_CONTROL_HOME/sftp/mole-interference-4004.bin"
: > "$MOLE_CONTROL_HOME/sftp/mole-boundary-4005-1048576.bin"
mkdir -p "$MOLE_CONTROL_DATA/webdav/mole-dav-4006"
mkdir -p "$MOLE_CONTROL_DATA/webdav/mole-dav-large-4007"
: > "$MOLE_CONTROL_DATA/ftp/Shared/mole-ftp-range-4008"
mkdir -p "$MOLE_CONTROL_DATA/smb/mole-smb-4009"
: > "$MOLE_CONTROL_DATA/nfs/mole-nfs-4010"
mkdir -p "$MOLE_CONTROL_MINIO/mole-multipart-4011"
sweep
exited 0
gone "$MOLE_CONTROL_HOME/sftp/mole-cut-4001.bin"
gone "$MOLE_CONTROL_HOME/sftp/mole-heavy-4002.bin"
gone "$MOLE_CONTROL_HOME/sftp/mole-upload-4003.bin.mole-partial"
gone "$MOLE_CONTROL_HOME/sftp/mole-interference-4004.bin"
gone "$MOLE_CONTROL_HOME/sftp/mole-boundary-4005-1048576.bin"
gone "$MOLE_CONTROL_DATA/webdav/mole-dav-4006"
gone "$MOLE_CONTROL_DATA/webdav/mole-dav-large-4007"
gone "$MOLE_CONTROL_DATA/ftp/Shared/mole-ftp-range-4008"
gone "$MOLE_CONTROL_DATA/smb/mole-smb-4009"
gone "$MOLE_CONTROL_DATA/nfs/mole-nfs-4010"
gone "$MOLE_CONTROL_MINIO/mole-multipart-4011"
# And it says how much, because a sweep that silently deletes gigabytes is its own
# kind of surprise -- and the count is the signal that runs are dying somewhere.
said "took 11 leftovers"
said "how often a run is dying"

# --- what it must not touch ---------------------------------------------------

begin "nothing outside the naming convention is touched"
fresh_tree
# Fixtures and the machine's own furniture. None of these carries a pid, which is
# what puts them outside the convention -- and `restore` is what removes the two
# that this program made.
: > "$MOLE_CONTROL_DATA/.mole-ballast"
mkdir -p "$MOLE_CONTROL_HOME/sftp/mole-many-files"
mkdir -p "$MOLE_CONTROL_DATA/ftp/Shared/mole-ftp-test"
: > "$MOLE_CONTROL_DATA/webdav/somebody-elses-file.bin"
: > "$MOLE_CONTROL_HOME/sftp/holiday-photos.tar"
mkdir -p "$MOLE_CONTROL_DATA/smb/notes"
sweep
exited 0
still "$MOLE_CONTROL_DATA/.mole-ballast"
still "$MOLE_CONTROL_HOME/sftp/mole-many-files"
still "$MOLE_CONTROL_DATA/ftp/Shared/mole-ftp-test"
still "$MOLE_CONTROL_DATA/webdav/somebody-elses-file.bin"
still "$MOLE_CONTROL_HOME/sftp/holiday-photos.tar"
still "$MOLE_CONTROL_DATA/smb/notes"
said "nothing left behind"
# "nothing left behind" and "nothing left behind, 0 in progress" say the same
# thing, and the second reads as though something had been spared.
if grep -qF "0 in progress" "$SCRIPT_OUTPUT"; then fail "counted nothing as something"; fi

begin "a directory that matches is taken whole, not walked into"
fresh_tree
# The shape a conformance run leaves: a working collection named for the run, with
# the run's own payloads inside it -- so the entries inside match the convention
# too. Without -prune both the directory and its contents are reported, the
# directory is removed first, and the count then says two runs died where one did.
mkdir -p "$MOLE_CONTROL_DATA/webdav/mole-dav-4020"
: > "$MOLE_CONTROL_DATA/webdav/mole-dav-4020/mole-upload-4020.bin"
: > "$MOLE_CONTROL_DATA/webdav/mole-dav-4020/mole-cut-4020.bin"
sweep
exited 0
gone "$MOLE_CONTROL_DATA/webdav/mole-dav-4020"
# One entry, not three: a directory reported once is what makes the count mean
# "runs that died" rather than "files somebody happened to write".
said "took 1 leftover,"

# --- a run in progress survives ----------------------------------------------

begin "a payload belonging to a run still going is spared"
fresh_tree
: > "$MOLE_CONTROL_HOME/sftp/mole-heavy-4030.bin"
: > "$MOLE_CONTROL_HOME/sftp/mole-heavy-4031.bin"
# 4030 is named as still running; 4031 is not.
sweep 4030
exited 0
still "$MOLE_CONTROL_HOME/sftp/mole-heavy-4030.bin"
gone  "$MOLE_CONTROL_HOME/sftp/mole-heavy-4031.bin"
said "run 4030 is still going"
said "kept 1 in progress"

begin "the pid is read from the name whatever else the name carries"
fresh_tree
# The boundary case carries two numbers -- the pid and the size -- and sparing it
# means reading the first. A sweep that read the last would delete the payload of
# the run that asked to be spared.
: > "$MOLE_CONTROL_HOME/sftp/mole-boundary-4040-1048576.bin"
: > "$MOLE_CONTROL_DATA/webdav/mole-dav-large-4040"
sweep 4040
exited 0
still "$MOLE_CONTROL_HOME/sftp/mole-boundary-4040-1048576.bin"
still "$MOLE_CONTROL_DATA/webdav/mole-dav-large-4040"
said "nothing left behind, 2 in progress"

begin "a payload a transfer still has open is spared without being named"
fresh_tree
: > "$MOLE_CONTROL_HOME/sftp/mole-heavy-4050.bin"
if command -v fuser >/dev/null 2>&1; then
    # Held open by a real process, which is the condition itself rather than a
    # clock. The `until` loop waits for the descriptor to exist instead of
    # sleeping: a test that sleeps 200 ms passes here and fails on a loaded machine.
    exec 9>>"$MOLE_CONTROL_HOME/sftp/mole-heavy-4050.bin"
    tries=0
    until fuser -s "$MOLE_CONTROL_HOME/sftp/mole-heavy-4050.bin" 2>/dev/null; do
        tries=$((tries + 1)); [ "$tries" -lt 100 ] || break
    done
    sweep
    exec 9>&-
    exited 0
    still "$MOLE_CONTROL_HOME/sftp/mole-heavy-4050.bin"
    said "a transfer has it open"
else
    # Reported, not skipped in silence: without fuser the sweep cannot tell a
    # transfer in flight from litter, and it says so.
    sweep
    said "no fuser here"
fi

# --- a dry run ---------------------------------------------------------------

begin "a dry run says what it would take and takes nothing"
fresh_tree
: > "$MOLE_CONTROL_HOME/sftp/mole-heavy-4060.bin"
mkdir -p "$MOLE_CONTROL_DATA/webdav/mole-dav-4061"
sweep --dry-run
exited 0
still "$MOLE_CONTROL_HOME/sftp/mole-heavy-4060.bin"
still "$MOLE_CONTROL_DATA/webdav/mole-dav-4061"
said "would take 2 leftovers"
said "would take $MOLE_CONTROL_HOME/sftp/mole-heavy-4060.bin"

# --- and it refuses nonsense -------------------------------------------------

begin "an argument that is not a pid is refused rather than guessed at"
fresh_tree
: > "$MOLE_CONTROL_HOME/sftp/mole-heavy-4070.bin"
sweep "everything"
exited 3
still "$MOLE_CONTROL_HOME/sftp/mole-heavy-4070.bin"
said "refusing"

# --- room and sweep agree about the layout ------------------------------------

begin "room answers for every service the sweep looks at"
fresh_tree
for service in sftp s3 webdav ftp smb nfs; do
    out=$("$CONTROL" room "$service" 2>&1)
    case "$out" in
    ''|*[!0-9]*) fail "room $service answered '$out' rather than a byte count" ;;
    esac
done

# --- the convention the sweep depends on --------------------------------------

begin "every pid-stamped name a suite creates carries the mole- prefix"
# The sweep tells our litter from anything else on the machine by the naming
# convention alone -- `mole-<what>-<pid>` -- because the runs it exists for died
# before they could write down what they had made. So the convention is the
# contract, and a suite that invents `probe-<pid>.bin` puts a payload somewhere
# nothing will ever collect.
#
# That is not hypothetical. The FTP root on the testbed still held `f16-<pid>.bin`,
# `fresh-<size>-<pid>.bin`, `probe-<size>.bin`, `t-<pid>.bin` and `rangeprobe.bin`
# from suites that have since been renamed -- three and a half megabytes that no
# sweep could match, because they were written before there was a convention.
offenders=$(grep -rn 'applicationPid()' "$MOLE_SOURCE_DIR/tests" "$MOLE_SOURCE_DIR/src" \
                 --include=*.cpp --include=*.h 2>/dev/null | grep -v 'mole-')
if [ -n "$offenders" ]; then
    fail "a pid-stamped name with no mole- prefix is a payload the sweep cannot find"
    printf '%s\n' "$offenders" | sed 's/^/    /'
fi

done_testing
