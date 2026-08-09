# The live test server

The suites that talk to a real server — SFTP, FTP, WebDAV, S3 — skip themselves
when there is nothing to talk to. This is how you give them something.

One virtual machine, built by one script, holding everything the backends speak
to. It is disposable by construction: nothing on it is worth keeping, and the
answer to any question about its state is to build it again.

## Building it

```sh
export MOLE_PROXMOX_HOST=…          # the hypervisor
export MOLE_TESTBED_PASSWORD=…      # a throwaway, for the test account
scripts/testbed/provision.sh
```

Neither value is in this repository, and neither should ever be. Both live in
the environment directory named in [CLAUDE.md](../../CLAUDE.md) under *Where
work is tracked* — this repository is public, and a host name typed into a
tracked file is the signal that a script wanted a parameter and got a constant.

Running it twice produces the same machine rather than a second one. Every
setting is applied on every run, so a machine that has drifted comes back to
what the script says it is.

## What it makes

| | |
|---|---|
| Machine | Proxmox VM `200`, named `mole-testbed`, cloned from a Debian 12 cloud-init template |
| System disk | 32 GB |
| Data disk | 4 GB, `ext4`, labelled `moledata`, mounted at `/srv/moledata` |
| Account | `moletest`, password from `MOLE_TESTBED_PASSWORD` |

The data disk is small on purpose. The WebDAV and FTP roots go on it, so *the
destination filled up* is a condition a test can create in seconds rather than
one it has to fake.

VM `200` is well clear of anything a person would have picked by hand. The
hypervisor is allowed to have other work on it, and this script touches nothing
but its own machine.

## Settings

Everything has a default except the hypervisor and the password.

| Variable | Default | |
|---|---|---|
| `MOLE_PROXMOX_HOST` | — | required |
| `MOLE_PROXMOX_USER` | `root` | |
| `MOLE_TESTBED_PASSWORD` | — | required; cannot contain a single quote |
| `MOLE_TESTBED_ADDRESS` | discovered | give it and the script stops guessing |
| `MOLE_TESTBED_VMID` | `200` | |
| `MOLE_TESTBED_NAME` | `mole-testbed` | |
| `MOLE_TESTBED_TEMPLATE` | `9000` | a cloud-init template with an ssh key in it |
| `MOLE_TESTBED_STORAGE` | `local-lvm` | |
| `MOLE_TESTBED_BRIDGE` | `vmbr0` | |
| `MOLE_TESTBED_CORES` | `4` | |
| `MOLE_TESTBED_MEMORY_MB` | `4096` | |
| `MOLE_TESTBED_SYSTEM_GB` | `32` | grown, never shrunk |
| `MOLE_TESTBED_DATA_GB` | `4` | set once, at creation |
| `MOLE_TESTBED_ACCOUNT` | `moletest` | |

**Give `MOLE_TESTBED_ADDRESS`.** The suite needs a stable address anyway, and an
address that is a parameter is one nobody has to discover twice. Without it the
script asks the guest agent, and failing that nudges the bridge's subnet and
reads the hypervisor's neighbour table — which works, but is a lot of machinery
to answer a question you already know the answer to.

## Not here yet

This script builds the machine and stops. What runs on it comes next:

- the four servers, and a second sshd that re-keys — issue #20
- the control channel for interfering with a running transfer — #21
- the snapshot to roll back to, and its name — #22
- `make test-live`, and a skip that says so — #23

## Taking it away

```sh
ssh $MOLE_PROXMOX_USER@$MOLE_PROXMOX_HOST 'qm stop 200 && qm destroy 200'
```

Nothing on the machine is worth keeping. That is the point of it.
