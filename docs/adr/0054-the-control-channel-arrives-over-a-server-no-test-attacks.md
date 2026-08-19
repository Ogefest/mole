# ADR-0054: The control channel arrives over a server no test attacks

- **Date:** 2026-08-19
- **Status:** Accepted

## Context

The interference tier exists to damage the test server while a transfer is
running: kill the connection, stop the service, fill the disk, shape the link,
take the network away entirely. None of that is worth having unless the machine
can be put back afterwards, and putting it back happens over `mole-control` —
which arrives over ssh, over the same network the tier is damaging.

That is a loop, and it closed twice. `netem loss 100%` on the root qdisc stops
the machine answering ARP, so it becomes unreachable to everything: to the
command that would clear the rule, and to the timer that was scheduled to clear
it. Both times the way back in was the hypervisor's guest agent, on a virtio
channel rather than over the network, which is now
`scripts/testbed/rescue.sh`.

The per-port answer — drop only what leaves the transfer's port — was written to
close the loop and did not, because of a fact nobody had written down: **the
control channel was on port 22, and port 22 is what the outage cases blackhole.**
`blackhole 22` cut the transfer and the channel together. The machine recovered
only because a timer on the machine itself happened to fire, which is to say the
instrument was safe as long as nothing went wrong. So the two cases that found
MOLE-108 sat behind `MOLE_TEST_INTERFERENCE_OUTAGE=1` for nine days, which in
practice meant they did not run.

Moving the channel to the second sshd on port 2222 does not fix it either. That
server is a target too: `hostkey rotate` changes its identity on purpose, and a
client that correctly refuses a changed host key would refuse the command that
restores it — the same loop wearing a different port number.

## Decision

**A third sshd exists on the testbed for the control channel alone, and nothing
in the tier is allowed to attack it.**

- `services.sh` provisions `sshd-control` on `MOLE_TESTBED_CONTROL_PORT`
  (2022 by default), with its own host key that is never rotated and a stock
  configuration that exists to be dull.
- `mole-control` reads that port from the server that serves it and **refuses**
  two commands outright: `blackhole` on its own port, and `service stop` on its
  own unit. Both exit 3 and say why.
- The unit is `Restart=always`, so anything that does kill it brings it back.
- `test-live.sh` and `test-heavy.sh` reach the channel there. `test-heavy.sh`
  refuses to run at all when that port does not answer, because it is the tier
  that blackholes port 22.
- `check-services.sh` asserts both halves: the port answers, and `mole-control`
  refuses to blackhole it.

The property this buys is held by a test rather than by this document:
`tst_Interference::theOutageCutsTheTransferAndNotTheChannel` cuts port 22 for
twenty seconds and asks the channel for its status throughout, requiring an
answer every time.

## Reason

The alternatives, and what disqualified each.

**Leave it on port 22 and rely on the scheduled undo.** This is what was there,
and it is not an instrument — it is a bet that nothing goes wrong at the one
moment designed to make things go wrong. The undo has already failed to be
scheduled once, silently, and the only reason that was ever noticed is that
somebody was watching.

**Move it to the second sshd on 2222.** It survives `blackhole 22`, and it dies
to `hostkey rotate`, which is a case in the same suite. It would also mean the
channel and the re-key transfers share a port, so the first future case that
blackholes 2222 reopens the loop without anyone noticing. Safety by coincidence
of which ports the current tests happen to attack is not safety.

**Run the control channel over the hypervisor's guest agent, like
`rescue.sh`.** Genuinely immune to everything, including a stopped sshd — but it
makes every ordinary interference depend on the hypervisor's credentials, drags
Proxmox into the test path, and is markedly slower per call in a suite that
polls. It stays what it is: the way back in when the machine is already lost,
not the way in during normal work.

**Have the tier blackhole a port other than the transfer's.** This is backwards.
The transfer's port is exactly what has to go dark for a total outage to be a
total outage.

The refusals matter as much as the port. A dedicated port that `mole-control`
would still happily blackhole if a test named it is a convention, and a
convention is one absent-minded argument away from another rescue over the guest
agent. The instrument declining to damage the path that undoes it is the part
that makes the tier something anybody can start and walk away from.

## Consequences

- The interference tier runs unattended. `MOLE_TEST_INTERFERENCE_OUTAGE` is
  gone, and the two total-outage cases run with everything else.
- The testbed has three sshds, and the reason each exists is different: port 22
  is the control cipher, 2222 provokes the re-key stall, 2022 is never touched.
  A fourth would need a reason of that kind.
- A machine provisioned before this ADR has no `sshd-control`. `control.sh` and
  `test-live.sh` fall back to port 22 and say so; `test-heavy.sh` refuses,
  because falling back there is the fault this record is about. Running
  `services.sh` again is the fix, and it is idempotent.
- `MOLE_TESTBED_CONTROL_PORT` joins the other testbed parameters. Like all of
  them it is read at run time and lives outside this repository.
- Two commands can now fail with exit 3 where they used to succeed. That is
  deliberate: a test that wants to blackhole the control port is a test with a
  mistake in it, and it should hear about it in a second rather than after a
  rescue.
