#!/usr/bin/env bash
#
# Builds the machine the live test suite runs against, from nothing.
#
# A machine nobody can rebuild becomes a machine with mysterious state that
# tests quietly depend on, so this is the only supported way to make one.
# Running it twice produces the same machine rather than a second one.
#
# Nothing about any particular installation is written down here. Where the
# hypervisor is, what the account is called and what its password is all arrive
# through the environment -- this repository is public, and a host name typed
# into a tracked file is the signal that a script wanted a parameter and got a
# constant. See CLAUDE.md, "Where work is tracked".
#
# Usage:
#   MOLE_PROXMOX_HOST=<hypervisor> MOLE_TESTBED_PASSWORD=<throwaway> \
#       scripts/testbed/provision.sh
#
set -euo pipefail

# --- what to build, and where -----------------------------------------------

# The hypervisor. The only setting with no sensible default: there is no such
# thing as a default machine to create servers on.
PROXMOX_HOST="${MOLE_PROXMOX_HOST:-}"
PROXMOX_USER="${MOLE_PROXMOX_USER:-root}"
NODE="${PROXMOX_HOST:+$PROXMOX_USER@$PROXMOX_HOST}"
# Well clear of anything a person would have picked by hand. The hypervisor is
# allowed to have other work on it, and this script must never touch it.
VMID="${MOLE_TESTBED_VMID:-200}"
NAME="${MOLE_TESTBED_NAME:-mole-testbed}"
# The Debian cloud image to build from. A path on the hypervisor, because that
# is where the machine gets made. Built from the image rather than cloned from
# somebody's template: a template is a machine whose history nobody can see, and
# the point of this script is that the history is the script.
IMAGE="${MOLE_TESTBED_IMAGE:-/var/lib/vz/template/iso/debian-12-genericcloud-amd64.qcow2}"
# Public keys to put on the machine, as a file on the hypervisor.
SSHKEYS="${MOLE_TESTBED_SSHKEYS:-/root/.ssh/authorized_keys}"
STORAGE="${MOLE_TESTBED_STORAGE:-local-lvm}"
BRIDGE="${MOLE_TESTBED_BRIDGE:-vmbr0}"
CORES="${MOLE_TESTBED_CORES:-4}"
MEMORY_MB="${MOLE_TESTBED_MEMORY_MB:-4096}"
SYSTEM_GB="${MOLE_TESTBED_SYSTEM_GB:-32}"
# Deliberately small. The WebDAV and FTP roots live here so that "the
# destination filled up" is a real condition a test can create rather than a
# fake one, and a few gigabytes fill in seconds.
DATA_GB="${MOLE_TESTBED_DATA_GB:-4}"
# The account the suite connects as. Throwaway by construction: the machine is
# disposable and holds nothing.
ACCOUNT="${MOLE_TESTBED_ACCOUNT:-moletest}"
# Where the machine will be. Given, the script stops guessing; left empty, it
# takes whatever DHCP hands out and goes looking. Giving it is much the better
# way round -- the suite needs a stable address anyway, and an address that is
# a parameter is one nobody has to discover twice.
ADDRESS="${MOLE_TESTBED_ADDRESS:-}"
PASSWORD="${MOLE_TESTBED_PASSWORD:-}"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
note() { printf '  %s\n' "$*"; }
die() { printf '\n%s\n' "$*" >&2; exit 1; }

# Derived from the id, in Proxmox's own address block, so a machine keeps its
# identity across every run of this script. See the note where it is used.
MAC="$(printf 'BC:24:11:00:%02X:%02X' $(( (VMID / 256) % 256 )) $(( VMID % 256 )))"

[ -n "$PASSWORD" ] || die "Set MOLE_TESTBED_PASSWORD. It is a throwaway on a
disposable machine, which is exactly why it is not in this repository: a
password in a public file is a password, whatever it protects."
case "$PASSWORD" in
    *\'*) die "MOLE_TESTBED_PASSWORD cannot contain a single quote: it is carried
into a shell on the far side, and quoting it correctly through two shells is a
trick nobody should have to read. Any other character is fine." ;;
esac

[ -n "$NODE" ] || die "Set MOLE_PROXMOX_HOST to the hypervisor, and
MOLE_PROXMOX_USER if it is not root.

The value is not in this repository on purpose. It lives in the environment
directory named in CLAUDE.md, under \"Where work is tracked\"."

# Everything on the hypervisor goes through here, so there is one place that
# knows how we reach it and one place to look when that changes.
on_node() { ssh -o BatchMode=yes "$NODE" "LC_ALL=C bash -s" -- "$@"; }

# --- the virtual machine ----------------------------------------------------

say "Hypervisor"
on_node <<'REMOTE' || die "cannot reach the hypervisor, or it is not Proxmox"
command -v qm >/dev/null || { echo "no qm on this host"; exit 1; }
pveversion | head -1
REMOTE

say "Machine $VMID ($NAME)"
if on_node <<REMOTE
test -f /etc/pve/qemu-server/$VMID.conf
REMOTE
then
    note "already exists -- leaving it where it is"
else
    on_node <<REMOTE || die "could not create $VMID"
set -euo pipefail
test -f "$IMAGE" || { echo "no image at $IMAGE"; exit 1; }
test -f "$SSHKEYS" || { echo "no public keys at $SSHKEYS"; exit 1; }

# The MAC is derived from the id rather than left to Proxmox to invent, and
# serial is the console rather than an extra -- without one there is no way to
# watch a machine that will not come up, which is exactly the state this script
# has to be able to get somebody out of.
#
# The MAC is the whole reason the first version of this script produced a machine
# that answered once and then went silent: re-applying "--net0 virtio,bridge=..."
# without a MAC makes Proxmox generate a new one every time, so each supposedly
# idempotent run gave the machine a new identity and threw away its DHCP lease.
# A setting that is re-applied must not change what it is applied to.
qm create $VMID --name $NAME --ostype l26 \
  --memory $MEMORY_MB --cores $CORES --cpu host \
  --scsihw virtio-scsi-single \
  --net0 virtio=$MAC,bridge=$BRIDGE \
  --serial0 socket --vga serial0 \
  --agent enabled=1 --onboot 0 \
  --description 'Mole live test server. Disposable: rebuilt by scripts/testbed/provision.sh.'

qm disk import $VMID "$IMAGE" $STORAGE --format raw >/dev/null 2>&1 \
  || qm importdisk $VMID "$IMAGE" $STORAGE >/dev/null
qm set $VMID --scsi0 $STORAGE:vm-$VMID-disk-0 >/dev/null
qm set $VMID --ide2 $STORAGE:cloudinit >/dev/null
qm set $VMID --boot order=scsi0 >/dev/null
qm resize $VMID scsi0 ${SYSTEM_GB}G >/dev/null

# The small disk. Its size is the point of it, so it is only ever created here.
qm set $VMID --scsi1 $STORAGE:$DATA_GB >/dev/null

qm set $VMID --ipconfig0 ip=dhcp >/dev/null
qm set $VMID --ciuser $ACCOUNT --cipassword '$PASSWORD' >/dev/null
qm set $VMID --sshkeys "$SSHKEYS" >/dev/null
qm cloudinit update $VMID >/dev/null 2>&1 || true
REMOTE
    note "built from $IMAGE"
fi
note "cores $CORES, memory ${MEMORY_MB}M, system ${SYSTEM_GB}G, data disk ${DATA_GB}G"

say "Starting"
on_node <<REMOTE
set -euo pipefail
if [ "\$(qm status $VMID | awk '{print \$2}')" != "running" ]; then
    qm start $VMID
fi
REMOTE

# The address comes from DHCP on the hypervisor's bridge, so it is discovered
# rather than assumed -- unless it was given, in which case there is nothing to
# discover and the machine is simply where it was said to be.
say "Address"
if [ -n "$ADDRESS" ]; then
    note "$ADDRESS (given, not discovered)"
fi
address="$ADDRESS"
for _ in $(seq 1 60); do
    [ -n "$address" ] && break
    address=$(on_node <<REMOTE || true
mac=\$(qm config $VMID | sed -n 's/^net0:.*virtio=\([0-9A-Fa-f:]*\).*/\1/p')
found=\$(qm guest cmd $VMID network-get-interfaces 2>/dev/null \
  | sed -n 's/.*"ip-address" *: *"\([0-9.]*\)".*/\1/p' | grep -v '^127\.' | head -1)
# Only fall through when the agent said nothing. An empty answer from a
# pipeline still succeeds, so the emptiness has to be tested rather than the
# exit status -- which is how this silently never reached the neighbour table.
if [ -z "\$found" ]; then
    # And the neighbour table only knows about machines something has spoken
    # to. A freshly booted guest with a lease is invisible here until somebody
    # sends it a packet, which is the other half of the same fault: the first
    # version read the table and found nothing, every time, for ever.
    subnet=\$(ip -4 -o addr show dev $BRIDGE | awk '{print \$4}' | head -1 | cut -d/ -f1 | cut -d. -f1-3)
    if [ -n "\$subnet" ]; then
        for host in \$(seq 1 254); do
            ping -c1 -W1 "\$subnet.\$host" >/dev/null 2>&1 &
        done
        wait 2>/dev/null || true
    fi
    found=\$(ip -4 neigh show | grep -i "\$mac" | awk '{print \$1}' | head -1)
fi
printf '%s' "\$found"
REMOTE
    )
    address=$(printf '%s' "$address" | tr -d '[:space:]')
    [ -n "$address" ] && break
    sleep 5
done
[ -n "$address" ] || die "the machine started but never appeared on the network"
note "$address"

# --- the account the suite connects as --------------------------------------
#
# Made through the hypervisor rather than over ssh, so this works before the
# machine will accept a connection from anybody but the template's own key.

say "Finishing the machine off"
ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$ACCOUNT@$address" \
    "sudo bash -s" <<'REMOTE' || die "could not finish the machine off"
set -euo pipefail
# The small disk, mounted where the servers that need to fill it will put their
# roots. Formatted once; a second run finds a filesystem and leaves it alone.
if ! blkid /dev/sdb >/dev/null 2>&1; then
    mkfs.ext4 -q -L moledata /dev/sdb
fi
mkdir -p /srv/moledata
grep -q '^LABEL=moledata' /etc/fstab || echo 'LABEL=moledata /srv/moledata ext4 defaults 0 2' >> /etc/fstab
mountpoint -q /srv/moledata || mount /srv/moledata
chown "$(logname 2>/dev/null || echo moletest)": /srv/moledata 2>/dev/null || true

# The guest agent, so the next run can simply ask the machine where it is
# instead of nudging a whole subnet to make it show up in a table. Installed
# here rather than baked into the template, because the template is not ours.
if ! systemctl is-active --quiet qemu-guest-agent 2>/dev/null; then
    DEBIAN_FRONTEND=noninteractive apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qemu-guest-agent >/dev/null
    systemctl enable --now qemu-guest-agent
fi

# Nothing else is installed here. What each protocol needs is the next issue's
# job, and a provisioning script that also configures four servers is a script
# nobody can read.
REMOTE

say "Ready"
note "machine   $VMID ($NAME) on $NODE"
note "address   $address"
note "account   $ACCOUNT"
note "data disk /srv/moledata (${DATA_GB}G)"
printf '\n  Put the address in the environment directory, not in this repository.\n'
printf '  Next: scripts/testbed/README.md, and issue #20 for the services.\n\n'
