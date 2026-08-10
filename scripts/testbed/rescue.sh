#!/usr/bin/env bash
#
# Puts the test machine back when it has stopped answering the network.
#
# The interference tier damages the link on purpose, and some of those damages
# cut the path that would undo them: `netem loss 100%` on the whole interface
# stops the machine answering ARP, so it is unreachable to the very command that
# would clear the rule -- including to the timer that was supposed to clear it,
# if that timer never ran. It happened twice while the tier was being written,
# and the only way back in both times was through the hypervisor.
#
# So this is that way in, written down. It goes over the guest agent, which
# travels on a virtio channel rather than over the network, so it works precisely
# when nothing else does.
#
# Usage:
#   MOLE_PROXMOX_HOST=<host> MOLE_TESTBED_VMID=200 scripts/testbed/rescue.sh
#   … scripts/testbed/rescue.sh reset      # last resort: a hard reboot
#
# Neither the host nor the id is in this repository. They live in the environment
# directory named in CLAUDE.md.
#
set -uo pipefail

HOST="${MOLE_PROXMOX_HOST:-}"
VMID="${MOLE_TESTBED_VMID:-200}"
ACTION="${1:-clear}"

if [ -z "$HOST" ]; then
    cat >&2 <<'MESSAGE'
Set MOLE_PROXMOX_HOST to the hypervisor, and MOLE_TESTBED_VMID if the machine is
not 200. Both live in the environment directory, not here.
MESSAGE
    exit 2
fi

on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "root@$HOST" "$@"; }

case "$ACTION" in
clear)
    # Everything the tier can leave behind, in the order that matters: the
    # traffic rules first, because until they are gone nothing else can be
    # reached to be asked about.
    echo "clearing traffic control on the guest…"
    on_host "qm guest exec $VMID -- /usr/sbin/tc qdisc del dev eth0 root" >/dev/null 2>&1
    on_host "qm guest exec $VMID -- /usr/bin/pkill -f /run/mole-netem-clear" >/dev/null 2>&1
    echo "removing any ballast and starting every service…"
    on_host "qm guest exec $VMID -- /usr/local/bin/mole-control restore" 2>&1 | sed -n 's/.*"out-data" : "\(.*\)".*/  \1/p'
    echo "done. The machine should answer again."
    ;;
reset)
    echo "resetting the machine — anything it was doing is lost"
    on_host "qm reset $VMID"
    ;;
*)
    echo "Usage: rescue.sh [clear|reset]" >&2
    exit 2
    ;;
esac
