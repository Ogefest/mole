# ADR-0018: Connected is not reachable, and nothing polls

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

The sidebar now lists drives rather than mounts, so every configured drive has a
row whether or not it is connected, and every row has to say what it is doing.
That means deciding what the words mean.

"Connected" is tempting to read as "working". It is not what the application
knows. `AppController::connectDrive()` asks a factory to build a backend from a
configuration and adds the result to the mount table. Building one performs no
I/O: an SFTP drive with the wrong host, a bucket that has been deleted and a
server that is switched off all connect exactly as successfully as one that
works. The first request finds out.

The other half of the question is how often to ask. A sidebar that showed live
reachability would have to keep asking, for every drive, for as long as the
window is open.

## Decision

`DriveListModel::State` distinguishes the two:

- **`Connected`** means a backend was built and mounted. It is a statement about
  this application, not about the far end.
- **`Unreachable`** means something *asked* and the far end did not answer. Only
  a check writes it.

Nothing polls. A drive's reachability is established when somebody asks for it —
opening the drive, or running the check explicitly — and the answer stands until
something asks again. `Connecting` and `Unreachable` therefore have no source in
this issue; the drive check supplies them.

## Reason

**Why not make `Connected` mean reachable.** It would be a lie for as long as it
took to notice, and the sidebar is exactly where somebody looks to decide
whether the problem is their network or their file. A row that says "Connected"
about a server that has been off since Friday is worse than a row that says
nothing, because it sends the reader looking somewhere else.

**Why no polling.** A liveness poll is a login attempt on somebody else's
server, repeated for as long as a window is open, for every drive they have ever
configured. On an SSH server with rate limiting that is a good way to get an
address blocked; on a metered S3 bucket it is a small bill for nothing. It also
cannot be done well: the interval that would notice a failure quickly is the
interval that costs the most, and the one that costs little tells you a drive is
fine when it has been gone for minutes.

**Why the states exist before anything writes them.** `Connecting` and
`Unreachable` are in the enum with no source yet, which normally deserves an
argument. Adding them later means every reader of the enum changes at once — the
model, the delegate, the palette — in a commit about something else. They cost a
case each now.

**The alternative that was rejected outright**: showing reachability by making
the sidebar open each drive at startup. That is polling with extra steps, and it
turns launching the application into a burst of connections to everybody's
servers.

## Consequences

- A drive can read `Connected` and fail on the next click. That is the honest
  state of knowledge, and the check is how somebody asks for better.
- Nothing in the sidebar costs network traffic on its own. Opening the window
  talks to nobody.
- When the check lands, it writes `Unreachable` and the meaning of `Connected`
  does not change — which is the point of separating them now rather than
  redefining a word later.
- A future "check them all" action is possible and is a deliberate, visible act
  rather than something the window does behind the reader.
