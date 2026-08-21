# Network drives

![The drives dialog](images/11-drives.png)

A drive is anywhere files live: a disk, an archive you have opened, or a server on
the other side of the network. Mole speaks **SFTP**, **FTP**, **WebDAV**, **S3** —
the last across AWS, Backblaze B2, MinIO, Ceph, Wasabi and Cloudflare R2 — and
**SMB** and **NFS**, which is what a NAS and a Linux or BSD file server offer. Once
one is set up it is a row in the same list as everything else. The listing, the
previews, the search, copying, renaming: none of them know or care which kind of
drive they are looking at.

**SMB and NFS depend on what Mole was built against** — `libsmbclient` for one and
`libnfs` for the other, both optional. A build without them does not offer that kind
at all rather than offering it and failing to connect, so the Kind list in the dialog
is the honest answer to what this copy of Mole can reach. If either is missing from a
build you did not make yourself, that is a question for whoever packaged it.

`F4` → File → Drives… opens the dialog above. It is where a drive is *configured*
and nothing else; connecting, ejecting and checking happen in the window.

## Adding one

Pick a kind, fill in the form, save. The forms are short on purpose — each
backend declares the fields it actually needs, so an SFTP drive asks for a host,
a user and how to authenticate, and an S3 one asks for a bucket and a key. There
is no page of eighty options, because the alternative to asking for what a
protocol needs is asking for the union of what every protocol might need.

Anything unusual sits behind **Advanced**, so the common case is four fields.

Saving also *checks* the drive — the answer to "did that work" appears next to
the form that produced it, rather than several steps later when something finally
tries to read from it.

## The passphrase

Passwords and keys are not written into the settings file. They go into a small
encrypted store, and that store is opened once with a passphrase you choose.

The part worth knowing: **the passphrase is not tied to this machine.** Nothing
about it is derived from the hardware, the user account or the installation. Back
up the configuration, take it to a fresh install, enter the same passphrase, and
the drives work. That is the whole reason it is a passphrase rather than
something the operating system keeps for you.

The settings file beside it stays readable, diffable and worth backing up — it
records *that* a field has a secret, never what the secret is.

**Nothing asks for it at startup.** The store is shut every time Mole starts, and
most sessions never touch a drive that needs it — so a drive whose password is in
there simply sits in the list wearing the hollow ring of a drive that is not
connected — which is what it is. Point at it and its tooltip says *Locked*, and the
button it offers is a key rather than a play triangle:

![A drive waiting on the passphrase](images/11d-drive-locked.png)

**Opening it is what asks.** Click the drive and the passphrase is asked for then,
in a dialog, because that is the first moment you have a reason to answer:

![Asking for the passphrase](images/11e-drive-unlock.png)

Enter it and the drive connects and opens — the click you made is finished, not
thrown away. Everything else that was waiting on the store connects at the same
time, so one entry is all it ever takes. A drive marked *connect at startup* that
has no password needs nothing typed at all.

See [ADR-0031](../adr/0031-a-locked-drive-is-connected-when-it-is-opened.md) for
why it works this way, including what it used to do instead.

## Connecting, ejecting, checking

A configured drive is in the sidebar whether or not it is connected. That is the
point of it being there: the list on the left answers *what can I get at*, and a
drive that only appears once it is working answers a different and less useful
question.

![A drive that is not connected](images/11b-drive-not-connected.png)

The row carries a dot for what the drive is doing, and one button that connects
or ejects it. A second asks the drive whether it is actually there. All three are
in the command palette too — `Ctrl+R`, then part of the drive's name — so none of
it needs the pointer.

![A drive that is connected](images/11c-drive-connected.png)

Connected, the row shows how full the drive is where the backend can say. Many
cannot: a bucket has no size in any useful sense, and neither does an archive. In
that case there is no bar rather than an invented number, because a bar is read
as a fact.

## What the states mean

The dot says what a drive is **doing**, and it says it the same way for every kind
of drive — a local disk, an open archive, a bucket, a server:

| Dot | State | Means |
|---|---|---|
| filled grey | **Idle** | here, and nobody is using it |
| filled blue | **Open** | one of your tabs is looking at something on it |
| breathing green | **Busy** | something you asked for is reading or writing it |
| hollow grey ring | **Not connected** | set up, not connected right now. Nothing is wrong. |
| hollow grey ring | **Locked** | its password is in the store, and the store is shut. The answer is the passphrase, not a button. |
| pulsing grey ring | **Connecting** | being connected, and nothing has heard back yet. |
| filled red | **Unreachable** | something asked and the server did not answer. |

Four things carry it, and each one carries a single idea. **Hollow against filled**
is *not here yet* against *here* — the pair that matters most, and one a shade of
grey cannot express at eight pixels. **Colour** is the kind: grey for nothing of
yours, blue for the one you are looking at, green for one being worked, red for
broken. **Motion** means *happening right now* — a deep breath while something is
being waited for, a gentle one while work is going through. A sidebar is furniture:
it has to be readable while it is being ignored, so nothing in it twitches. And **no
dot at all** means the row is not a drive: that is how a bookmark looks.

The legend you end up with without being handed one: *grey is here and quiet, blue
is the one I am looking at, green and breathing is one being worked, hollow is not
connected yet, red is broken.*

**Busy** answers a question people ask out loud — *which of my drives is this copy
actually touching?* A copy has two ends and usually two drives, and both of them
light up. Only work you asked for counts: Mole asks every drive how full it is once
a minute, and if that counted, every row would stir once a minute for ever.

**Nothing is green for merely being connected.** Under this scheme "connected" is
*Idle* — available and unused — and a colour of its own for it would be a
celebration of nothing happening. What says a connection worked is the row's own
word changing, the ring becoming a solid dot, and the capacity bar appearing. Green
is kept for the one thing worth that much attention: a drive being worked.

**Idle** and **Unreachable** are worth reading carefully, because they say less
than they look like they say.

Connecting to a drive builds the machinery to talk to it, which involves no
network traffic at all — a drive pointed at a server that has been switched off
connects exactly as successfully as one that works. So a solid dot here means
something has actually reached the far end, not merely that Mole has an object for
it. That is why connecting runs a check, and why the row sits at *Connecting* —
pulsing, and still hollow — until the check answers.

*Unreachable* means the opposite: something asked and got nothing back, at the
time shown on the row. It does **not** mean the drive has gone. The connection is
still there and the next operation may well work — what failed was a question,
and the row reports it rather than pretending to know more. An unreachable drive
can still be browsed.

Nothing polls. A drive's state changes when something is actually learned about
it — you walked into a folder on it, or you pressed check — and between those moments the row
shows what was last true and when. A liveness poll would be a login attempt on
somebody else's server repeated for as long as the window is open, which is a
good way to get an address rate-limited and a small bill on a metered bucket, for
information nobody asked for.

## What one drive can do and another cannot

Drives are not interchangeable, and pretending they are throws away the only thing
some of them have. A disk on a filesystem that keeps earlier states of a file knows
what those states are; a container that keeps earlier objects under one key knows
the same; an object store can hand out a link to one object that works for a while
without an account. None of that is anything the other drives can do, and none of it
is anything Mole would otherwise have anywhere to put.

So a drive can offer things of its own. What it offers appears under **Operations**
in the menu, alongside everything else that acts on the file in front of you, in the
drive's own words. Mole does not know what any of it means: it shows what the drive
listed, hands the choice straight back, and does one of two things with the answer.

**What is on offer depends on what the drive was pointed at, not only on which kind
of drive it is.** The same local drive has earlier versions on one filesystem and not
on another; the same object store has them on one container and not on another. Mole
finds out by asking the drive, once, the first time you open a folder on it — so a
drive you never open is never asked, and opening one costs a single question that the
listing does not wait for.

### Reading an earlier version

A row is marked when the drive has something for that file. Opening the file's
earlier versions gives a list to choose from, and choosing one **opens it as an
ordinary file** — which is the whole trick: every viewer in
[Looking inside files](previews.md) works on it, because it is a file like any other
and not a special screen. A large one is read a page at a time over the network, the
same way the current one is.

Inside the preview there is a picker beside the file name. It says **current** until
you move it, and it says which version you are on afterwards — always, whether or not
the drive has anything else to offer. A preview that showed you an earlier version
while looking like the file itself would be worse than not having the feature.

Copying one out is copying a file: the version you are looking at is what a copy,
a drag or *Copy path* acts on.

### Nothing here writes

**Mole shows what a drive already holds, and takes a copy out of it.** It does not
roll a file back, delete an earlier version, change how long they are kept, or turn
the feature on. A filesystem that keeps snapshots has a tool for managing them and a
container has a console; if that is what you want, that is where it lives. What Mole
adds is being able to *look*, in the same window as everything else, without going
and finding those tools first.

### When a drive offers nothing

Which is most drives on most machines, and it costs nothing at all. No mark on any
row, no extra entries in the menu, no picker in the preview, and no question asked of
the drive beyond the one that found out it had nothing to say. A plain disk on an
ordinary filesystem browses exactly as it did before any of this existed.

## What is not here

**Google Drive, Dropbox and OneDrive are not backends and are not planned as
part of the application.** Each needs OAuth, a registered application, token
refresh and a change-notification model of its own; they have almost nothing in
common with each other beyond the words "cloud storage". They belong in plugins,
which is what the drive extension point is for. The reasoning is in
[ADR-0011](../adr/0011-network-drives-without-rclone.md), which is also where the
question of why this is not a wrapper around rclone is answered.
