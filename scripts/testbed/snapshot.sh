#!/usr/bin/env bash
#
# The snapshot the live suite starts from, and how it gets back to it.
#
# The destructive scenarios are the point of this machine -- filling a disk,
# deleting trees, killing services -- so every run has to start from the same
# place. Without that, the second run of a suite is testing whatever the first
# one left behind, and the day it fails nobody can say which run caused it.
#
# Usage:
#   MOLE_PROXMOX_HOST=… scripts/testbed/snapshot.sh take     # once, after provisioning
#   MOLE_PROXMOX_HOST=… scripts/testbed/snapshot.sh rollback # before and after a run
#   MOLE_PROXMOX_HOST=… scripts/testbed/snapshot.sh list
#
set -uo pipefail

PROXMOX_HOST="${MOLE_PROXMOX_HOST:-}"
PROXMOX_USER="${MOLE_PROXMOX_USER:-root}"
NODE="${PROXMOX_HOST:+$PROXMOX_USER@$PROXMOX_HOST}"
VMID="${MOLE_TESTBED_VMID:-200}"
# One name, used everywhere. A snapshot whose name is a date is a snapshot
# nobody can write a script against.
SNAPSHOT="${MOLE_TESTBED_SNAPSHOT:-provisioned}"
ADDRESS="${MOLE_TESTBED_ADDRESS:-}"
ACCOUNT="${MOLE_TESTBED_ACCOUNT:-moletest}"

heading() { printf '\n\033[1m%s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }

[ -n "$NODE" ] || { echo "Set MOLE_PROXMOX_HOST. It is not in this repository." >&2; exit 2; }
[ $# -ge 1 ] || { echo "Usage: snapshot.sh take | rollback | list | delete" >&2; exit 2; }

on_node() { ssh -o BatchMode=yes "$NODE" "LC_ALL=C bash -s"; }

# The machine has to be answering before a run starts, or the suite skips
# itself and reports green for having done nothing.
wait_for_ssh() {
    [ -n "$ADDRESS" ] || return 0
    for _ in $(seq 1 60); do
        ssh -o BatchMode=yes -o ConnectTimeout=4 -o StrictHostKeyChecking=no \
            "$ACCOUNT@$ADDRESS" true 2>/dev/null && return 0
        sleep 2
    done
    echo "the machine never came back on $ADDRESS" >&2
    return 1
}

case "$1" in
take)
    heading "Taking $SNAPSHOT of $VMID"
    on_node <<REMOTE
set -e
qm listsnapshot $VMID | grep -q ' $SNAPSHOT ' && qm delsnapshot $VMID $SNAPSHOT >/dev/null 2>&1
# With the memory, so a rollback lands on a machine that is already running its
# servers rather than one that has to boot and be waited for.
qm snapshot $VMID $SNAPSHOT --vmstate 1 --description 'Provisioned and seeded. scripts/testbed/snapshot.sh'
REMOTE
    note "taken"
    ;;

rollback)
    heading "Rolling $VMID back to $SNAPSHOT"
    on_node <<REMOTE
set -e
qm rollback $VMID $SNAPSHOT
# A rollback of a snapshot with memory leaves it running; one without does not.
sleep 3
[ "\$(qm status $VMID | awk '{print \$2}')" = "running" ] || qm start $VMID
REMOTE
    wait_for_ssh || exit 1
    note "back, and answering"
    ;;

list)
    on_node <<REMOTE
qm listsnapshot $VMID
REMOTE
    ;;

delete)
    on_node <<REMOTE
qm delsnapshot $VMID $SNAPSHOT
REMOTE
    note "deleted $SNAPSHOT"
    ;;

*)
    echo "Usage: snapshot.sh take | rollback | list | delete" >&2
    exit 2
    ;;
esac
