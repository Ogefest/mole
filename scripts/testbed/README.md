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

Running it twice produces the same machine rather than a second one, and leaves
it exactly as it was — including its address.

That is worth stating plainly because the first version of this script got it
wrong in a way that took a while to see. It re-applied `--net0 virtio,bridge=…`
on every run, and Proxmox invents a fresh MAC when it is not given one, so every
"idempotent" run handed the machine a new identity and threw away its DHCP
lease. The machine answered once and then went silent for ever. The MAC is
derived from the machine id now, and settings that would change what they are
applied to are not re-applied at all.

The machine gets a serial console for the same reason. Without one there is no
way to watch a machine that will not come up, which is precisely the state
somebody needs to get out of:

```sh
ssh -t $MOLE_PROXMOX_USER@$MOLE_PROXMOX_HOST 'qm terminal 200'
```

## What it makes

| | |
|---|---|
| Machine | Proxmox VM `200`, named `mole-testbed`, built from a Debian 12 cloud image |
| System disk | 32 GB |
| Data disk | 4 GB, `ext4`, labelled `moledata`, mounted at `/srv/moledata` |
| Account | `moletest`, password from `MOLE_TESTBED_PASSWORD`, plus the hypervisor's public keys |
| Console | serial, so a machine that will not boot can be watched |
| Guest agent | installed, so later runs can ask the machine where it is |

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
| `MOLE_TESTBED_IMAGE` | a Debian 12 genericcloud qcow2 under `/var/lib/vz/template/iso` | on the hypervisor |
| `MOLE_TESTBED_SSHKEYS` | `/root/.ssh/authorized_keys` | on the hypervisor; goes on the machine |
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

## The servers

`provision.sh` builds the machine and stops. `services.sh` puts the four
protocols on it, and `check-services.sh` asks each one whether it is actually
there:

```sh
export MOLE_TESTBED_ADDRESS=…
export MOLE_TESTBED_PASSWORD=…
scripts/testbed/services.sh
scripts/testbed/check-services.sh
```

| | | |
|---|---|---|
| SFTP | port 22 | OpenSSH, `aes256-gcm` — a sixteen-byte block, so it does not re-key at 2^30 |
| SFTP | port 2222 | a second OpenSSH, `chacha20-poly1305` and `RekeyLimit 256M` |
| WebDAV | `/dav` | Apache `mod_dav`, root on the small disk |
| FTP | port 21 | vsftpd, root on the small disk, passive 30000–30020 |
| S3 | port 9000 | MinIO, store on the system disk |

The two sshds exist to be **different**. The stall in
[ADR-0013](../../docs/adr/0013-a-large-sftp-read-arrives-in-spans.md) is a
property of a server's configuration, so the fix has to be held against a server
that provokes it and one that does not. `check-services.sh` compares what the
two actually negotiate and fails if they match — which it did, immediately, the
first time it was run: OpenSSH prefers `chacha20-poly1305` by default, so both
servers had ended up identical and the "second server" was a second port.

The WebDAV and FTP roots are on the small disk so that *the destination filled
up* is a condition a test can create. MinIO's store is not: a bucket has no
business filling the disk that condition is measured on.

## Running the live suites

```sh
export MOLE_TESTBED_ADDRESS=…
export MOLE_TESTBED_PASSWORD=…
make test-live
```

Every `MOLE_TEST_*` variable the suites read is derived from those two, so there
is one place to change when the machine moves.

**A skip is a result.** Each of these suites skips itself when there is nothing
to talk to, and in practice that meant they had never run — which is how a
listing behaviour that differs between servers stayed hidden until a second
server was tried. A suite that skips is named and counted here, and the run is
not green.

The testbed's FTP certificate is honestly self-signed, so the run sets
`MOLE_TEST_IGNORE_SELF_SIGNED_CERT`. TLS stays *required*: that variable says
who vouches for the certificate, not whether the connection is encrypted.

## Interfering with it on purpose

```sh
MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh install
MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh fill 95
MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh netem loss 30% 15
MOLE_TESTBED_ADDRESS=… scripts/testbed/control.sh restore
```

The difference between a server and a piece of test equipment is whether a test
can do something to it *while it works*. `mole-control` on the machine can stop
and start a service, kill established connections without stopping the server,
fill the small disk to a chosen percentage and empty it, and apply or remove
`tc netem`. Every command prints what it did, because a test that fails after
interfering has to be able to say what it interfered with.

From C++ it is `TestbedControl` in `tests/support`, and **it is absent by
default**: nothing reaches for it unless `MOLE_TEST_CONTROL` names the command,
so a suite on somebody's own machine cannot start stopping services on anything.

```sh
export MOLE_TEST_CONTROL='ssh -o BatchMode=yes moletest@<address> sudo mole-control'
```

**Every `netem` clears itself after thirty seconds by default.** That is not
tidiness. This channel reaches the machine over the network it is being asked to
damage, so `netem loss 100%` cuts off the very command that would undo it — the
machine goes unreachable, including to whoever is trying to put it back, and
only a reboot from the hypervisor recovers it. That happened once while this was
being written. The timer means a run that dies mid-test leaves a machine that
heals itself.

## Not here yet
- the snapshot to roll back to, and its name — #22
- `make test-live`, and a skip that says so — #23

## Taking it away

```sh
ssh $MOLE_PROXMOX_USER@$MOLE_PROXMOX_HOST 'qm stop 200 && qm destroy 200'
```

Nothing on the machine is worth keeping. That is the point of it.
