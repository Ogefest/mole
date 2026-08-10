# Done

Finished work, newest first, with a line on how each one was resolved. Anything
still outstanding lives in [TODO.md](TODO.md).

This is not a changelog for users — it is the record of what was asked for and
what the answer turned out to be, including the ones where the first answer was
wrong.

---

## WebDAV met a real server, and could not write to it (#35)

**Asked for:** the WebDAV backend has never been run against a live server, and
has since grown a streaming write that has never been exercised — a large write
goes out with a chunked transfer encoding, and a server answering 411 would
refuse it. Run the conformance suite and a large streaming write against a real
server, green.

**What it turned out to be:** far worse than the issue expected, and nothing to
do with chunked encoding. The first large write returned

    Writing /large.bin.mole-partial: necessary data rewind wasn't possible

`CURLAUTH_ANY` — chosen so a user never has to care whether their server wants
Basic or Digest — makes curl send the first request with **no credentials**, take
the 401 and try again. The retry has to send the body a second time, and the body
comes from a `QIODevice` curl has no way to rewind. Meanwhile `Expect:` was being
appended to every request, switching off the one mechanism that would have made
the 401 arrive *before* the body did.

**It was not a large-write problem.** Reproduced with plain
`curl --anyauth -T -` against the same server: exit 65, `CURLE_SEND_FAIL_REWIND`,
at every size tried from one kilobyte upwards. Every WebDAV write of any real
size had been failing against any server that asks for a password. The only
reason the suite was green is that the conformance fixtures are a few bytes each
— small enough for curl to hold a copy and send it again.

The fix is one condition: keep `Expect: 100-continue` for a request that carries
a file, and suppress it only for the ones that do not. curl asks only when it
needs to, so a directory of small files pays nothing.

Two tests, both live, and both fail on the old code for the reason above: a
96 MiB write that asserts it took the streaming route, and an 8 MiB write that
asserts it took the staged one. Each checks the route with a `dynamic_cast`
rather than trusting a size threshold — a threshold quietly raised past the test's
file would otherwise turn one of them into a duplicate of the other, passing while
covering nothing.

**The lesson is about the fixture, not the backend.** A conformance suite that
only ever writes a few bytes tests a different code path from the one users
take, and it had been green for months over a backend that could not write a
file. Sizes that cross a real boundary — an authentication retry, a staging
threshold, a buffer — belong in the suite deliberately.

## An upload killed mid-flight left a partial file that looked finished (#33)

**Asked for:** an upload interrupted by the process being killed leaves part of a
file on the server, and what it leaves looks like a finished file. Write to a
temporary name and rename on success.

**What it turned out to be:** exactly what the issue said, and the interesting
part was where the fix could not go. [ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md)
already handles an upload that *fails* — the backend deletes what it wrote — and
that answer covers every failure the process is alive to see and none of the ones
it is not. `SIGKILL` runs no destructor and no error path. So no amount of
cleanup code was ever going to help; the protection had to be the name the bytes
were travelling under all along, which needs nothing to survive because it is
already on the server. That is [ADR-0020](docs/adr/0020-an-upload-in-progress-wears-a-different-name.md).

`StreamingUpload` and `BufferedUpload` gained a commit hook — one function, run
after everything has arrived and nothing has gone wrong, whose failure becomes
the stream's failure. It does not run for a stream that was cancelled or
abandoned, which is the case that matters: part of a file has gone up, and giving
it the real name would be the fault itself. SFTP, FTP and WebDAV write to
`<name>.mole-partial` and rename at the end. **S3 was deliberately left alone** —
a single `PUT` is atomic and a multipart upload does not exist as an object until
it is completed, so there is nothing there to protect.

**The test was the harder half.** A transfer that fails can be faked; a process
that is killed cannot be, because the whole point is that no code of ours runs
afterwards. So the live test spawns *this same binary* with an environment
variable telling it to be the victim, waits for the file to appear on the server
and sends it `SIGKILL`. It waits on the file appearing rather than on a clock,
and — this took a second attempt — it waits for the file under **either** name.
The first version watched only for `.mole-partial`, so against the unfixed
backend it sat there for ninety seconds while the victim uploaded two gigabytes,
and then reported "the upload never reached the server, so nothing was killed
mid-flight". Which was untrue, and was the least useful thing it could have said:
the whole question is which name the bytes are under, so both are worth waiting
for and the answer is what gets asserted. Watching for either, it fails in three
seconds with `a killed upload left mole-killed-555030.bin, which looks like a
finished file` — the fault, named.

Two things were found while writing it rather than being designed in. A `stat` of
the destination that fails for a reason other than "not there" was originally
treated as "the name is free"; it is now treated as "could not find out", because
guessing there is guessing about whether somebody else's file is about to be
replaced. And a rename that fails now removes the working file, so a failed
upload still leaves nothing behind — bytes under a name nothing will ever open
are litter, not a result.

## Only part of a large file arrived from an SFTP drive

**Asked for:** work out why files copied from an SFTP server to local disk came back
as fragments, and why large files threw timeouts. A log to diagnose it with was
asked for alongside, and then broadened: every task and every drive should report
itself the same way, whatever the question being chased.

**What it turned out to be:** an SFTP transfer stops dead a little short of a
gibibyte. The bytes arrive at full speed and then simply cease, with the connection
open and the server still there; two minutes later the stall guard gives up. Whether
that presented as "a timeout" or as "half a file" depended only on which layer the
caller was watching.

It is not ours. Plain `curl` does it with no Mole involved, and every attempt to
place the blame closer to home was disproved by measurement:

| what was done | result |
|---|---|
| `curl`, one transfer, fresh process | 1,211,049,311 bytes, complete, 53 MB/s |
| `curl`, a listing and then the file | stops at 1,072,635,904 |
| `curl`, a small file and then the file | stops at 1,072,635,904, the same byte |
| Mole, pooled connection | stops at 1,071,513,600 |
| Mole, `CURLOPT_FRESH_CONNECT` | stops at 1,071,529,984 |
| Mole, a brand-new easy handle | stops at 1,071,529,984 |
| `scp`, any size | fine, always |

Two wrong answers were held on the way, and both were killed by the table above. The
first was connection reuse -- the pool hands out warm connections, and the failures
all happened to follow a listing. A fresh connection stops in the same place. The
second was handle reuse, since curl's own failures shared an easy handle across two
URLs. A brand-new handle stops in the same place too.

What survives is the byte offset. Every failure lands just short of 2^30, which is
where an OpenSSH server re-keys the session when the negotiated cipher has a block
size under sixteen bytes -- `chacha20-poly1305` here. `libssh`, which this
distribution's curl is built against, says nothing at all when it happens: the trace
ends mid-transfer and resumes with the stall guard firing. OpenSSH's own client
re-keys without trouble, which is why `scp` is untroubled, and why this belongs to
the pairing of the two rather than to either end.

**The answer:** a read larger than 256 MiB arrives a span at a time, by byte range,
each span over a connection of its own, appended to the same local copy. No
connection carries enough for the re-key to arrive. A file that fits in one span is
fetched exactly as before, over a pooled connection -- an SSH handshake costs 0.58 s
here, measured, which is ruinous across ten thousand small files. Telling those two
cases apart is what `expectedSize` on `openRead()` is for: `TransferTask` and
`SyncTask` pass what their plan already measured, everything else says nothing and
gets the careful path. See
[ADR-0013](docs/adr/0013-a-large-sftp-read-arrives-in-spans.md). The 1.2 GB file that
could not be read at all now arrives in five spans in 65 seconds.

**Two ways the same fault was being hidden, both fixed:**

- **A short transfer was handed over as a whole file.** `net::errorFor` now compares
  what the server announced against what arrived, for every protocol, and fails the
  read with both numbers in the message. It fires only when the server itself stated
  a length and the request asked for a body, so a `HEAD` is unaffected.
- **A source that stopped responding looked like the end of the file.** The copy loop
  read until a read returned nothing, which `QIODevice` also does when a read fails,
  and then reported success. It now checks whether the device is actually at its end.

**The log**, since none of this could be seen from outside: four categories by
subject -- `mole.task`, `mole.drive`, `mole.net`, `mole.curl` -- turned on with
`MOLE_LOG=net,curl` or `MOLE_LOG=all`, writing into the session log that already
exists. Two of them are written in one place each and cover everything by
construction: `Task::execute` for every job, and a `LoggingFileSystem` wrapper that
every mount goes behind for every drive. Silent at debug and audible at warning, so a
short download or a failed job leaves a line whether anyone asked for logging or not.
Credentials in header lines are redacted, because a log gets sent to other people.
See [ADR-0012](docs/adr/0012-a-log-you-can-turn-up.md).

**Then the staging, which was the other half of it.** Every network backend read a
file by downloading all of it into a temporary file first, and wrote one by
collecting all of it before sending any. For the files this was about -- 94 GB, 20
GB, several at 4 GB -- that is not slow, it is impossible: a copy to a drive with
room to spare needed 94 GB of local scratch space on a machine with 84 GB free, and
twice that between two remote drives. So SFTP now streams in both directions, in the
same spans, with a bounded buffer between the transfer and the caller: about 8 MiB
per direction whatever the file weighs. Measured on a 1.2 GB read: not one byte of
temporary space. A 1.5 GB upload through the new write stream came back byte for
byte. Progress also moves from the first second now, because bytes go through the
copy rather than into a temporary file first, and a preview of a huge remote file
reads its first page instead of fetching the lot. Files up to 64 MiB are still
fetched whole, where random access is free and the preview layer wants it. See
[ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md).

**Then S3 and WebDAV, which had been left staging on the grounds that their
protocols require it.** Both reasons are real and neither survives a file larger than
the local disk. S3 now sends an object it cannot measure as a multipart upload --
begin, 64 MiB parts, complete -- staging one part at a time so each can be signed,
and dropping back to a single PUT when the payload turns out to fit in the first
part. That also lifts the 5 GB ceiling a single PUT imposed: the new one is 640 GB.
A failed upload is abandoned rather than left to be charged for, and completion is
read out of the response body, because S3 answers that request with 200 and puts the
failure inside the document. WebDAV streams a large write with a chunked transfer
encoding and keeps the staged PUT for everything else, so the servers that refuse
chunked are only met in the case that has no alternative. Verified against Backblaze
B2: a 150 MB object goes up in parts and comes back byte for byte, with the
conformance suite still green for everything smaller. See
[ADR-0015](docs/adr/0015-s3-uploads-in-parts-webdav-in-chunks.md).

**And then the question of who says a copy worked.** Everything up to here was our
own account of what we sent: bytes handed to a backend, no complaint, stream closed.
So now every copied file is weighed where it landed -- one listing per destination
directory rather than a stat per file, because on SFTP a stat *is* a listing of the
parent -- and a file whose size there is not the number of bytes that went into it
fails the copy. It runs before a move deletes anything, so a move to a destination
that lost bytes no longer destroys the only good copy. A check that cannot be run is
logged rather than treated as a failure. See
[ADR-0016](docs/adr/0016-a-copy-is-weighed-at-the-destination.md).

**Two more faults found by running the suite against the real server**, neither
related to the above:

- **Listing a file reported "not found" on this server rather than "not a
  directory".** Asked to list a path with a trailing slash, some servers answer with
  a "." row describing the file and others say it does not exist -- which is true of
  the directory asked for and false of the file that is there. `list()` treated "not
  found" as unambiguous and returned it; it now falls through to the same check that
  handles the other answer. The live conformance suite passes against this server for
  the first time.
- **A forward seek on a stream raced the transfer filling it.** Whether a short hop
  forwards was answered from the buffer or started a new connection depended on how
  much had arrived, which made the behaviour a matter of network timing. A hop within
  the buffer's length now waits for those bytes and skips them; further than that
  still starts again.

**What is not fixed** and is now in TODO.md: an upload interrupted by the process
being killed leaves a partial file that looks finished -- writing to a temporary name
and renaming on success would close that; the WebDAV write path has still never met a
real server; and FTP still stages, which nobody is blocked on.

---

## Copying a location to the clipboard

**Asked for:** copy the path of the open folder, of the selected file, or of the
drive root.

**What it turned out to be:** three separate menu entries rather than one that
guesses. They copy three different things, and a single action that sometimes took
the file and sometimes the folder would be a coin toss whose result is only visible
after it has been pasted somewhere. The file entry is disabled when a folder is
under the cursor instead of quietly copying that folder.

What gets copied is the **native path** for anything on local disk and the **uri**
for anything else. A remote drive has no native path, and handing out the path part
alone would produce something that looks local and is not: `/reports/2026` pasted
into a terminal means a directory that does not exist rather than a folder in a
bucket.

`Ctrl+Shift+F` for the file, not the `Ctrl+Alt+C` some file managers use: Ctrl+Alt
is AltGr on Polish and many other layouts, where AltGr+C is a letter people type.

The methods return the text they copied, so the tests assert what would be copied
without needing a clipboard, and the ones that do have a QGuiApplication check the
real clipboard as well.

**Two faults found on the way, both of which made the feature look broken:**

- **The menu can advertise a key that does nothing.** A `MenuAction`'s `shortcut`
  is only what the menu prints beside the title; the binding is a separate `Shortcut`
  in Main.qml. Naming the key without declaring it would have shipped a menu entry
  that lied. The walkthrough test presses the keys rather than trusting the field.
- **A notification stopped every shortcut in the application.** The toast was a
  `Popup`, whose default close policy includes Escape -- so it wanted key events, and
  while it had them no window shortcut fired for the five seconds it was up. It
  looked exactly like the second copy key being unbound, and it was not: the first
  press always worked and every press after it did nothing, whichever key it was.
  The toast now closes on a click outside and on its timer, and asks for no keys.
  Pressing a shortcut twice in a row is what catches this, so the test does.

---

## Sidebar rows stopped being cramped and stopped twitching

**Asked for:** drive and bookmark rows are low for something that behaves like a
button, and the label shifts when the pointer crosses the row -- a jumping label
that looks bad.

**What it turned out to be:** two separate faults with one appearance.

The name label fills whatever space its siblings leave, and both siblings were
bound to hover: the "free" caption was hidden on hover and the × was shown on
hover. So crossing a row changed the label's width twice, and it re-elided in
place. The × now keeps its place at all times and only fades in and out, and the
caption stays put -- so nothing beside the name moves and the name has nothing to
move for. The test hovers a real row with a real mouse event and asserts the
label's x and width are unchanged.

The heights were worse than "low". A plain row was 30 pixels around a button that
is 28 on its own, and a drive row with a capacity bar was fixed at 46 while the
content inside it wanted 54 -- it was being squeezed, which is its own share of why
the rows looked wrong. Heights now come from the content with a floor of
`minimumTarget + 8`: 36 for a bookmark, 57 for a drive with a capacity bar. The
row that holds the name is pinned to the target size so it measures the same with
or without a ×, because the drives list is mixed -- a local disk cannot be ejected
and an archive can -- and two row heights in one list read as a bug.

Found while writing the test: `hovered` never became true under it, because
`Control.hoverEnabled` follows a platform style hint. The highlight, the tooltip
and the × all quietly depended on how the platform felt about hover effects, so it
is now stated outright.

---

## A drive is checked where it is configured

**Asked for:** verify the configuration when it is saved. The complaint was
precise — with a wrong parameter the road to finding out was far too long — and it
came from hitting it: an S3 drive against Backblaze B2 failed with *"SSL: no
alternative certificate subject name matches target host name"*, several steps
away from the form that caused it.

**What it turned out to be:** nothing in the application ever asked the far end
anything. `connectDrive()` only built the backend, and building one performs no
I/O, so a drive that could not work looked exactly like one that could until
something tried to read from it. Saving now runs a `DriveCheckTask` that lists the
drive's root — the cheapest request that proves name resolution, TLS, credentials
and the path all at once — and reports either what it found or why it could not.
The answer arrives as a notification and as a band across the drives dialog, and
every configured drive has a button to ask again.

A Task rather than a call, because `IFileSystem` is synchronous and worker-thread
only: a blocking check against a host that is not answering would freeze the
window for the whole timeout. The drive is saved either way, so a failed check
never costs what was typed.

Two details worth keeping:

- **The verdict is a band across the dialog, not a line in the form.** Saving
  clears the form, so a line inside it vanished at the exact moment it became
  worth reading.
- **It reports a count, not "connected".** An empty answer from the wrong place
  looks identical to an empty answer from the right one, and the number is what
  lets someone tell them apart.

Found while writing the walkthrough test for it: the band appeared, carried the
right text, and was zero pixels wide, because it was measured on the frame it
became visible rather than after the layout had run.

**Also fixed, from the same report:** a bucket whose name contains a dot cannot be
addressed through the host name at all — a wildcard certificate covers exactly one
name part, so `my.backups.s3.…` fails TLS however it is configured. Such a bucket
now always goes in the path, and an endpoint that already carries the bucket is not
given it a second time. Both were verified against B2's real certificate rather
than reasoned about.

---

## rclone out, four network backends in

**Asked for:** drop rclone — too much ballast for what it gave, and its
configuration had become far too complicated — and put SSHFS, SFTP, FTP, S3 and
WebDAV in its place as a plugin, with room for more backend plugins later. The S3
one had to work against AWS, Backblaze B2 and anything else S3-compatible.

**What it turned out to be:** four backends, not five. SSHFS was dropped on the
author's decision once it was pointed out that it is SFTP over FUSE: the files
should be reachable from inside Mole and not mounted into the operating system,
and FUSE would not port to Windows anyway. So a drive stays virtual and
in-application, and SFTP covers talking to an SSH server.
[ADR-0011](docs/adr/0011-network-drives-without-rclone.md) records the whole
decision, including what is genuinely lost: Google Drive, Dropbox, OneDrive and
Mega speak proprietary APIs that none of these protocols reach.

One dependency serves all four: **libcurl**, which speaks sftp, ftp, ftps and
https. The deciding argument was not its feature list but the threading contract —
`IFileSystem` is synchronous and worker-thread-only, and libcurl's easy interface
is blocking, where `QNetworkAccessManager` would have needed an event loop pumped
on every worker. aws-sdk-cpp was rejected for repeating rclone's mistake at a
different scale; what was wanted from it was SigV4, and that is a page of
HMAC-SHA256 over the OpenSSL already required for the credential store.

Verified against real servers rather than only in principle: SFTP, FTPS and S3 all
pass the full conformance suite live — SFTP and FTPS against an Egnyte account, S3
against Backblaze B2 — with the fixtures seeded through plain libcurl rather than
through the backend under test, so a bug could not cancel itself out. The live
tests are driven by `MOLE_TEST_*` variables and skip when there are none, so the
suite is green on a machine with no account and no credential is committed. WebDAV
has no live server to hand and is covered by parser tests against Nextcloud- and
Apache-shaped answers, with the conformance run waiting on
`MOLE_TEST_WEBDAV_URL`.

Five things went wrong on the way, and each one is now a test:

- **A file listed as a directory read as an empty folder.** Asked to list a regular
  file, an SFTP server does not refuse — it answers with a `.` entry describing the
  file. The parser was dropping dot entries, so browsing into a file showed an
  empty directory instead of an error.
- **The signer did not sign the headers it produced.** `x-amz-date` and
  `x-amz-content-sha256` were sent but left out of the signed set, which worked in
  a test harness that passed them by hand and was refused by B2 with "header must
  be included in signature". The signer adds them itself now.
- **Signing took a pre-encoded, pre-sorted query.** The first thing to get that
  wrong was this project's own cross-check harness, which is a fair verdict on the
  interface. The signer now owns the encoding, and the url that is sent is built
  from the same encoder.
- **FTP's success code read as a failure.** A finished listing ends with 226
  "Transfer complete"; interpreted as an HTTP status that is an error. Protocols
  that report through the transfer result now say so explicitly.
- **Every WebDAV timestamp came back empty.** `getlastmodified` is an HTTP date
  ending in `GMT`, which Qt's RFC 2822 reader rejects, and it starts with a weekday,
  which Qt validates — so a server whose weekday arithmetic is a day out would have
  its timestamps thrown away. The weekday is now dropped before parsing.

Two problems found in existing code while wiring the backends up, both invisible
until a backend buffered its writes:

- **A failed upload looked like a successful copy.** `TransferTask` and `SyncTask`
  called `close()` on the write stream and ignored the outcome, and every remote
  backend commits in `close()`. `closeAndReport()` now collects it through a small
  `ICommitsOnClose` interface, and a failed send is reported as a failed transfer.
- **`CompressTask` never closed its stream at all**, resetting the device instead,
  so packing an archive onto a remote drive would have written nothing while
  reporting success.

Also hardened on the way past: `saveDrive` used to write any value it was handed
into the readable settings file, including keys no backend had declared. An
undeclared key is now dropped, so nothing can smuggle a secret into a file that is
meant to be read — no field would have marked it secret.

---

## User documentation

A guide in [docs/guide/](docs/guide/README.md): an index, then browsing, looking inside
files, finding things, the operations, and the command palette. Written for someone
using Mole rather than someone building it, which is why it is separate from
ARCHITECTURE.md and from the ADRs.

The part worth having is the rule about its pictures. Every screenshot comes from
`make screenshots`, which drives the real application headlessly and photographs each
state the walkthrough test has *just asserted* — so a picture cannot outlive the feature
it documents. Twenty-eight of them exist because twenty-eight states are checked;
`make guide-images` regenerates them and copies them in, so refreshing the guide after a
change is one command rather than an afternoon with a screenshot tool.

A script checks that every image and every internal link in the guide resolves, because
a guide with a broken picture is worse than one with no pictures: it looks maintained.

README.md gained a *Using it* section pointing at the guide, and its list of things not
yet done lost PDF, SQLite and Parquet — all three are built, and leaving them on a wish
list would have been the same kind of lie the screenshot rule exists to prevent.

## The palette had never actually offered the drives

Reported: searching commands should find the available drives too, because they sit in
the list on the left and reaching one means clicking rather than typing.

They were supposed to be there already — the palette is built as a view over the action
registry, the bookmarks *and* the mount list, precisely so it cannot drift out of step
with them. The test said otherwise at once: five drives in the sidebar, none in the
palette.

The cause was ten lines of ordering. The palette was constructed with a pointer to the
mount list before that list existed, so it held null for the lifetime of the process and
offered no drives at all. Everything else about the palette worked, which is why this
survived: the failure is silent, and an empty category looks exactly like a category
nobody has anything in.

The test is the useful part, and it does not check that *a* drive is offered — it checks
that every drive in the sidebar is, because the palette holds no list of its own and
anything missing is something the mouse can reach and the keyboard cannot. It then types
a drive name and follows the choice through to the navigation it asks for. Putting the
old order back makes it fail.

## Telling the two buttons in a dialog apart

Reported: on the popups it is not clear which button is the active one, they look the
same, and it is easy to make a mistake.

Measured, it was worse than that. The two buttons were flat labels of identical size,
weight and colour, **and neither of them held the keyboard** — so there was no active
one to see. Over the delete dialog they read *Yes* and *No*, which says nothing about
which one deletes.

Now: the button that acts is filled and the one that backs out is outlined; the fill is
red when the thing cannot be undone; both are labelled with the verb — *Delete*,
*Compress*, *Rename*, *Create* — and a destructive dialog opens with the keyboard on
the safe way out, outlined so it can be seen. A stray Return closes the question rather
than answering it. Dialogs that are typed into keep the keyboard in the field.

The instructive part was the Material style. Asking it for a highlighted button gives
an item that reports itself visible, correctly sized, and filled with the colour asked
for, and paints nothing. Every property said red. What settled it was counting red
pixels in the screenshot: zero. The backgrounds are drawn by hand now, and the test
reads the colours off the items rather than the properties that had lied.

Two process notes worth keeping. My direct runs of the test binary do not write
screenshots — only the `screenshots` target does — so for a while I was looking at a
stale image and drawing conclusions from it. And the focus took three attempts because
a `DialogButtonBox` is three focus scopes deep: the popup, the box, and the ListView
the box lays its buttons out with. Miss the middle one and the focus is set, reported
as set, and never active.

See [ADR-0010](docs/adr/0010-telling-the-two-buttons-apart.md).

## Compressing can take the originals with it

Asked for as a checkbox: pack this, and leave me the archive without the files.

One operation to a person, two to a file manager -- and the middle step, finding the
same selection again to delete it, is where the wrong thing gets deleted. So the
feature is easy and the rules around it are the work, because this is the only thing
here that deletes data as a side effect of something else.

The archive is the only copy once the originals go, so nothing goes until the archive
is provably a complete copy of them:

- **After the archive is written and closed**, never as part of writing it. There is
  no moment with the files gone and no archive.
- **Nothing is deleted if anything could not be read.** One unreadable file inside a
  packed folder means the archive is missing it, and the original is the only place it
  exists. Both are kept and the status says which case it was.
- **Nothing is deleted if the job was cancelled**, even when the archive survived.
- **A source containing the archive is never deleted.** Packing the folder you are
  standing in writes the archive inside it, so deleting that source would take the
  archive with it -- turning "keep the archive, drop the files" into keeping nothing.
- **A deletion that fails is reported, not fatal.** The archive is written and correct,
  which is the part that cannot be repeated.

The box is off every time the dialog opens. Archiving something once and dropping the
originals is not a standing instruction.

Three tests cover the three refusals, and the guards were checked by removing them:
without the first, the unreadable-file case deletes a folder the archive does not fully
contain; without the second, the archive deletes itself along with its folder. Both
failures are real data loss in the temporary tree, which is what makes them worth
having tests for.

Moving to a trash instead was considered and left alone: it belongs to all deletion in
the application rather than to compression, and a trash that some drives have and
others do not is worse than none.

See [ADR-0009](docs/adr/0009-packing-can-delete-the-originals.md).

## Dialogs that destroy something say what they are aimed at

Reported plainly: the delete popup does not show which files it is about to delete,
"and that could be a problem in a moment".

It could. Four dialogs stood in front of something irreversible and all four asked
with a number — *"Permanently delete 2 items?"*, *"7 files, 2.4 GB"*, *"12 files at
the destination will be removed"*. A number only confirms what somebody already
believes. It cannot catch the case the dialog exists for: that the operation is aimed
at something other than what they think.

That case is not hypothetical here. Ticks survive navigating away and coming back, the
cursor is a target when nothing is ticked, and a selection made in one folder is easy
to still be carrying in another. Each of those is a route to a correct-looking count
over the wrong files.

So every one of them now lists the entries by name, through one shared component
rather than four hand-rolled copies. Compressing had grown its own list first; a
second copy would have been the point where the two started saying the same thing
differently. Deleting duplicates lists full locations instead of names, because inside
a duplicate group every name is identical and the location is the only thing that
tells them apart. Syncing lists only the deletions, since a plan can be thousands of
steps and what is being agreed to here is the part that destroys something.

Two details that are the whole point. The list comes from the same call that performs
the operation — `FileListModel::targetEntries()`, which `targets()` is now built from
— because two functions answering "what is selected" is exactly how a list and an
action drift apart. And it is taken when the dialog opens rather than bound live: a
refresh landing or a watcher firing behind an open dialog must not change what
pressing Yes means. That second one is tested by mutation — binding it live instead
makes the test fail.

One correction on the way through: I read a scaled-down screenshot as a clipped
warning line and "fixed" the dialog's height for it. Measuring said the content was
114 pixels in a 209-pixel space and had never been clipped; the change was reverted
rather than left in as a fix for nothing.

See [ADR-0008](docs/adr/0008-naming-what-an-operation-touches.md).

## Compressing: 7z, bare xz, and a name that was being thrown away

Two formats added and one bug fixed, and the bug was mine.

**The name was being discarded.** Reported as: a name typed into the box, and the
archive written under the original one. The cause was the *Kind* picker — changing the
format rebuilt the whole name from the selection, silently replacing whatever had been
typed, while the comment above it claimed it "renames the suffix". It does that now:
the base is kept, including any dots in it, and only the suffix is swapped. The test
covers the multi-part suffixes that make this easy to get wrong (`holiday.tar.gz` →
`holiday.7z`, not `holiday.tar.7z`), case, dots in the middle, and an empty box giving
nothing rather than a file called `.zip`.

**7z is now offered**, which reverses the first version of ADR-0007. That exclusion
rested on libarchive writing less of the format than it reads; measured on 3.7.2, a
multi-entry 7z writes and reads back correctly, so the caveat did not apply to what is
actually being done here.

It cannot be encrypted, though, and that came with a trap worth recording. This
libarchive rejects `7zip:encryption` as an undefined option — yet it accepts a
passphrase anyway, returns success, and the written file contains no plain text,
because LZMA2 compressed it rather than anything encrypting it. A test looking only for
the plain bytes would have called that encryption. So a password is refused for 7z, and
whether a format can carry one is a stated fact rather than an inference from the
output.

**A bare `.xz`** is one compressed stream with no container: one file, no folders.
Asked for with more, it refuses before writing anything, rather than failing on the
second entry with *"Raw format only supports one entry per archive"*.

Its test took three attempts, each corrected by measurement rather than by guessing.
Asserting an entry list found none — correctly, since a bare xz keeps no names.
Asserting the plain text was absent failed, because eleven bytes are not compressible
and LZMA2 stores an input that small uncompressed inside a perfectly valid stream.
Asserting a tenfold reduction failed too: libarchive pads its output to a
ten-kilobyte block, so 56 kB of repetitive text comes out at 10 kB of which most is
padding. Checked with `xz -t` and `xz -dc` on the way through — the stream is valid and
returns every original byte — and the bound in the test is deliberately loose so nobody
tightens it into a flake.

## Compressing: what it packs, and a password

Two changes, and the first was not the one it looked like.

The report was that a selection should be compressed rather than the folder. Measuring
first: a *ticked* file already was — `currentTargets()` returned it and the suggested
name followed it. What was empty was the case where a file is only **under the cursor**,
unticked, which fell back to the whole folder. Compressing was the one operation here
that ignored the cursor: copying with `F5`, deleting with `F8` and analysing all use the
commander rule of "ticked entries, or the row under the cursor". It now does too, and
the folder in view applies only when there is no row at all.

Since a rule about what gets packed should not have to be inferred, the dialog now says
what it is aimed at — *notes.txt*, *2 selected items*, *the folder documents* — and
**lists the entries by name**, with folders marked as folders. A count is a summary; the
point of a dialog before an operation is being able to see it is pointed at the right
things. The list scrolls and stops growing at a sensible height, so ticking forty files
does not produce a dialog taller than the window.

A password protects a zip with AES-256, not zip's original scheme, which is broken and
known to be. It is offered only where the format can carry one: a tar has no notion of a
password and gzip and xz encrypt nothing, so the box is disabled with a line saying why.
A passphrase handed to the task for such a format is **refused**, not ignored — someone
who typed a password and received an archive anybody can open has been quietly lied to,
which is worse than an error.

The test for it checks the thing that matters rather than the thing that is easy: that
the plain text is not sitting in the written file, and that the archive reader cannot
hand the contents back without the password. An implementation that accepted a password
and wrote everything in the clear would pass any assertion short of that.

## Compressing files and folders

An operation on the selection — or on the folder in view when nothing is ticked —
that asks the two things worth asking, a name and a kind, and packs in the background
like every other job that takes time. The archive lands beside what was packed, which
is where anyone would look for it.

Three formats: zip, tar.gz, tar.xz, with zip the default because it is the one anyone
can open anywhere without being told how. 7z is deliberately absent — libarchive
writes less of that format than it reads, so offering it would mean sometimes
producing an archive something else refuses. Reading 7z is unchanged.
[ADR-0007](docs/adr/0007-writing-archives.md) has that and the rest.

Both ends go through `IFileSystem`, so packing a selection on a remote drive is the
same code as packing one on local disk, and archive mounts stay read-only: this writes
a new archive and nothing else. Adding to an existing one sounds like a small extension
and is not — it means rewriting the container and having something sensible to do when
the process dies half way — so it is not offered.

A cancelled or failed compression deletes the partial file. An archive that exists is
one that finished; a half-written file waiting to be mistaken for a good one is the
worst outcome available here. One unreadable file is recorded and skipped rather than
fatal, the way the walker treats a directory it cannot enter — the rest of the archive
is still worth having, and what was missed is said out loud.

Where the code lives took a correction. The writer went into
`mole_archive_backend`, which already links libarchive, and the define was put on
`mole_builtin` — but `AppController` lives in `mole_ui`, *below* the built-ins, so
`canCompress()` compiled to `false` and the walkthrough test skipped itself while
looking like it had run. The link belongs where the operation is; `mole_ui` sits on
`mole_core` exactly as the backend does, so nothing is inverted.

The tests pack a tree and read it back through the archive *reading* backend, in all
three formats, so the writer and the reader hold each other to account rather than the
writer being checked against its own idea of what it wrote. The cancellation test
needed a second version: waiting to observe `Running` and then cancelling is a race the
task can win, and under a loaded machine it did once — it now packs from a drive that
is slow to open files and cancels on the first sign of progress, which lands mid-write
every time.

And then a test failed about one run in three, which turned out to be the most useful
thing that happened here. It was not flakiness: the entry header was written *before*
the file was opened, so a file that then could not be read left a header promising N
bytes with nothing behind it — corrupting everything after it in the stream. It only
showed when the walker happened to reach the unreadable file before a good one, which
is exactly why it looked intermittent. The file is opened first now, and the test names
the unreadable file ahead of the readable one instead of leaving the order to the
walker, so the case that found the bug is checked every time rather than a third of the
time.

## Search results were a list you could only look at

Three things they could not do, and one of them was in the wrong place.

Building a set from them existed, beside the criteria — which is not where the rows
are. It has moved to a strip above the results, along with the two other things worth
doing to a match: showing it in its folder, and looking at it without leaving. The
index search shows the same strip without the set button, since it has nowhere to put
one yet.

Walking them was impossible: the list had no focus handling at all, so results could be
read and double-clicked and nothing else. Arrows move now, Enter goes there, F3 previews,
and Down out of the query box walks into the answers — once a search has answered, the
answers are where the keyboard should be. Arriving at a fresh list puts the cursor on the
first row rather than nowhere, which took a second attempt: results arrive after the view
exists, and a model that had no rows leaves `currentIndex` at -1.

"Show me where this is" is the natural end of most searches and had no way to be asked
for. `revealFile()` opens the folder holding a file *with the cursor on the file* —
arriving in the right folder with the cursor somewhere else is only half an answer. The
listing lands asynchronously, so the pane remembers what it was asked to reveal and
consumes it when the entries arrive; a file already in the current folder needs no
navigation and is selected straight away. Both paths are tested, because the second is
the one that quietly does nothing if forgotten.

## Previews had no options, and nowhere to remember one

An `.html` file previewed as coloured source. Sometimes that is what someone wants and
sometimes they want to read the page, which makes it a setting — and there was nowhere
to put a setting: `SessionStore` remembered which tabs were open and the window
geometry, and nothing else about anything.

Three answers, all in [ADR-0006](docs/adr/0006-preview-options-and-preferences.md).
`Preferences` is one small file of dotted keys that knows nothing about what they mean.
A provider *declares* its options — key, title, choices, default — and the strip above
the preview renders them without knowing what any of them are, the same way the menu
renders entries from plugins it has never heard of. And a choice is keyed by provider
*and* suffix, because "the next `.html`" is what was asked for and one text viewer
serves `.html`, `.xml` and `.svg` with different sensible answers; the provider id keeps
two viewers claiming a suffix from overwriting each other.

The choice applies immediately and is remembered, and it is applied *before* the file
is read, so opening the next `.html` shows the page straight away rather than showing
source and then correcting itself.

The rule that mattered most is the one about the network. Qt's rich text engine
resolves what a document names, so a page could quietly tell whoever wrote it that a
file had been looked at — in a file manager that is a nasty surprise, not a feature.
Anything a document could reach out with is removed before it is rendered: images,
scripts, stylesheets, frames, embedded objects, event handlers. Blunt rather than
clever, because telling a local reference from a remote one means parsing and resolving
and getting that subtly wrong is exactly the failure being prevented. The test feeds it
a deliberately hostile page and asserts that not one `http` survives while the words do.

Two mistakes worth recording. The first is mine and it leaked: adding a store without
teaching `PrivateProfile` about it meant the tests wrote into
`~/.local/share/Mole/mole-tests/preferences.json` — real user data, outside the sandbox
— which is why one test passed alone and failed in the suite, reading back what a
previous *run* had left. `MOLE_PREFERENCES_PATH` is in the profile's list now, the
leaked file is gone, and the suite was run twice to prove it. Adding a store means
teaching the test profile about it, or the tests are not isolated at all.

The second was the same delegate-recreation trap as bulk rename: republishing the option
list rebuilds the Repeater's delegate, so the test's pointer to the picker was dangling
and reading it hung the run. The test looks the item up again instead.

## A long search froze the interface

Reported as: a search that runs for a while eats so much CPU that the window stops
responding — and, tellingly, a folder analysis running alongside it took *longer*
and did nothing of the kind.

That comparison was the clue that mattered, because it ruled out the walk. The
analysis walks the same trees through the same `DirectoryWalker` and reports its
status just as often. What it never does is put anything into a model the interface
is watching.

`FileListModel::appendEntries()` reset the model and called `rebuildVisible()`,
which copies every entry found so far, filters it, and `stable_sort`s the lot. On
every batch. A search returning forty thousand results in batches of two hundred
therefore sorted a growing list two hundred times, on the thread that draws the
window. Measured before touching anything: **9,670 ms** of pure CPU for that case.

The batch is now filtered and sorted on its own and `std::inplace_merge`d into what
is already in order — both halves share the comparator, so the result is sorted
without looking at the earlier entries again. The same case now takes **246 ms**,
which is thirty-nine times less work in front of the person waiting.

Worth saying plainly: the time-based flushing added an hour earlier made this worse
in exactly the case reported. Before it, batches only went out every two hundred
matches; after it, also every hundred and twenty milliseconds — so a long search
produced more batches, and each batch cost a full re-sort. The latency fix was right
and the quadratic append underneath it was the bug; together they were the freeze.

The test states the case rather than the mechanism — forty thousand results in two
hundred batches, and a ceiling generous enough for a debug build on any machine —
and it fails with the number in the message, which is how the 9,670 ms above was
measured in the first place.

One thing deliberately left: a reset still discards the view's scroll position and
selection, so scrolling through results while they arrive is unsatisfying. Fixing that
means proper insert semantics rather than a reset, which is a bigger change than the
freeze warranted; it is recorded in TODO.md.

## Search results arrived late, and led nowhere

Two halves, and the first turned out to be one number.

`LiveSearchTask` batched matches at two hundred before emitting them — and only at
two hundred. A search over a large tree that matched a dozen files therefore showed
nothing at all until the whole walk had finished, which is exactly the case anyone
searching a disk meets. Batches now go out on whichever comes first, enough matches or
a hundred and twenty milliseconds, so the first answers arrive almost immediately and a
flood still costs one signal per two hundred rather than one per file.

The test for that was the interesting part. The suite already had
`streamsResultsWhileRunning`, which only checked the totals once everything had
finished — it proved nothing about arriving early, and it passed before and after. The
first replacement was no better: asserting a batch arrived while `isFinished()` was
false passes even with count-only batching, because the last flush happens inside
`run()` before the task is marked finished. Only a clock can answer *when*, so the test
now times the first batch against the whole walk and fails with a sentence that says
what went wrong: *first matches arrived at 1511 ms of a 1511 ms walk*.

The second half is what results are for. They can be narrowed where they are —
straight onto the model that already holds them, so no walk and no query, just less of
what is there — with a count that reads "3 of 41" when a filter is on. And they can
become a file set, which is where the work carries on: a snapshot of what is on screen,
narrowing included, because the rows in front of someone are what "these results"
means. A set that re-ran the query later would be a different promise from the one the
button makes. Nothing to build from produces no set rather than an empty one, and an
unnamed set is named after the query rather than after nothing.

## Ctrl+F was not usable as a search box

Three things, and only the last is a feature.

The keyboard was not in the field, so the first thing anyone did after pressing
`Ctrl+F` was reach for the mouse to click into it — which is exactly what the key is
supposed to save. It is focused now, on creation and whenever the shell asks the tab
for its pane.

Enter already started a search; nothing said one was *running*. A tree walk over a
large disk takes long enough that silence reads as nothing having happened, so the
results area now says it is searching while there is nothing to show, and steps aside
the moment rows arrive — the same threshold-free rule the table preview uses, because
here the walk streams matches from the start.

Then the criteria. Size, typed the way people write it: `10M`, `1.5 GiB`, `500k`,
`1,5M` with a comma, because that is a decimal point in most of Europe and this
application already shows sizes that way. Nothing and nonsense both mean "no limit"
rather than zero — a limit of zero bytes would quietly match nothing. It lives in a
*More* section that is folded away, so the common case stays one field and one key.

The interesting part was the index, and it needed a decision rather than code:
[ADR-0005](docs/adr/0005-which-engine-answers-a-search.md). The form now asks the
index when an indexed volume's root is a prefix of the folder being searched, and
walks otherwise. Partial coverage counts as none — the temptation was to ask the index
for the part it covers and walk the rest, which would produce one list where some rows
are current and some are as old as the last scan, with nothing to say which. The
toggle is on by default because the index is enormously faster and usually right, and
what makes that default safe is that the status line always names the engine that
answered and how old the index is. Turning it off is the case that matters: the truth
on disk right now, whatever the index remembers.

Both engines already had `minSize`/`maxSize`, which is why size was the criterion
added first — anything the index cannot express would have to fall back to walking and
say so.

Tested at both levels: the size parser on its own including the cases that must mean
"no limit", the engine choice as behaviour (unindexed walks, indexed answers from the
index, the toggle forces a walk on a file written after the scan), and the box itself
in the real window — the field holds the keyboard on opening, five typed characters
reach the controller, and a 500M floor empties a fixture that has nothing that big.

## A dist/ from before the rename failed the licence check

`make licence-check` had been failing on *bundled Qt cannot be replaced by the user*
since before any of the recent work, and it had nothing to do with licensing. A
`dist/` sat in the tree from a `make bundle` run made when the binary was still called
`superfilemanager`; the check looked for `dist/mole`, found nothing, and reported the
bundle as non-replaceable. Everything that actually mattered passed throughout — Qt
dynamically linked, no Qt symbols in the binary, no GPL-only module.

Seventy-six megabytes of git-ignored build output holding the old binary and copied
system libraries, with nothing hand-made in it. `make bundle` already begins with
`rm -rf dist`, so it was a stale artefact rather than a bug in the target, and deleting
it was the fix. The tree is left without a bundle, which is how a fresh checkout looks:
one is built on demand, and a bundle left lying about is precisely what caused this.

The change worth keeping is the message. Ten minutes went into working out what
"bundled Qt cannot be replaced" meant, so the check now names the launcher it wants —
*there is no launcher at dist/mole (stale bundle? run: make bundle)* — and separates
the three ways replaceability can fail instead of reporting one verdict for all of
them. A fresh `make bundle` was run end to end to confirm the check still passes when
there is something real to check, including the replaceability test that had never
actually run.

## The header says the palette is there

A shortcut nobody has been told about is a feature nobody has. So the title bar now
carries something that looks like the box it opens — a search glyph, the words *Search
commands*, and `Ctrl+R` drawn as a key — in the middle of the window, on the same line
as the hamburger and the name.

It opens the palette rather than trying to be one. Two boxes that both filter would
mean two places owning the same state, and the reason the palette works is that one
place owns the list.

Centred in the *window*, which took a second attempt: laid out in the toolbar's row
between the menu on one side and the task indicator on the other, it sat noticeably
right of centre, because equal spacers centre a thing between its neighbours and not
in the window. It is anchored to the toolbar instead. The test holds all three claims
that make it work as a teaching aid — visible, within two pixels of the window's
middle, on the same line as the menu button — and that clicking it opens the palette.

## The palette moved to Ctrl+R, and stopped remembering the last query

Three small things, and one of them was only found by trying to break the test.

The box kept whatever was typed into it last: `onAboutToShow` reset the model's
filter but not the field, so the next opening showed a list narrowed by a query the
user could no longer see a reason for. It clears the field now.

`Ctrl+R` belonged to Refresh, which was the wrong use of a key that good — refreshing
is one row in the palette like everything else, and it keeps its View menu entry. Its
`shortcut` label went with the binding, because a menu that advertises a key that no
longer works is worse than one that advertises nothing.

And the palette lost its animations. That started as a test problem — pressing the key
again straight after Escape did nothing, because `opened` goes false when the exit
transition *starts* and `open()` during that transition is silently ignored — but it is
a real one: a human closing and reopening quickly would hit exactly the same wall. A
box you summon to type one word into should be there the instant you ask.

The test was worth more than the fixes. Removing `field.clear()` and running it again
showed it still passing, which meant the assertions I thought I had written were not in
the file at all — the edit had not matched. Written properly, and checked the same way,
it now fails with `"termi"` still sitting in the box. It also has to check the clearing
*before* running a command, because the command it runs is the terminal, and a terminal
that holds the keyboard stops `Ctrl+R` reaching the window — which is ADR-0002 working
exactly as intended, in a place I had not expected to meet it.

## One input that can reach everything

`Ctrl+Shift+P` opens a box with a list underneath of everything that can be done
right now — the whole `F4` menu tree, every bookmark, every drive. Typing `termi`
leaves one row, *Operations → Terminal here*, and Enter runs it. Arrows move, Escape
leaves, and nothing about it needs the mouse, which matters because the reason it
exists is that not every control has a shortcut of its own.

The design decision that makes it worth trusting is that it holds no list. The menu
entries come from `ActionRegistry::buildModel()`, the places from `BookmarkModel` and
`MountListModel`; the palette is a view over those three. A second list maintained by
hand would drift out of step with the menu the first time somebody added an action,
and then the one thing the palette promises — that it has everything — would quietly
stop being true. The test that says so is the first one in the file: the palette's
paths must equal what the menu would show, entry for entry.

"Only what is available" came for free rather than needing a mechanism: the menu
already evaluates each entry's `enabled` callback at the moment it is asked, so a
greyed-out action is simply absent. It is rebuilt on every open, because what can be
done depends on the tab in front of the user.

Ranking is less optional than it looks. A title match beats a match on the group, or
typing `set` buries *Add to set* under everything in a section whose name contains
those letters; and several words match anywhere in the path in any order, so both
`op term` and `term op` find the terminal. Each of those is a test, because each is
a way for the box to feel broken while technically working.

The model asks and the shell acts — `actionRequested` and `locationRequested` rather
than a call into tabs or navigation — which is what lets it stay a plain view. The
walkthrough proves the whole path in the real window: the key opens it, the input has
the keyboard immediately, five characters narrow everything to one row, and Enter
opens the terminal.

## PDFs had no preview

A PDF fell through to the information viewer — the last resort for a file we cannot
show — so previewing one gave its size and its type and nothing of its contents,
while the listing already drew it an icon and `IPreviewProvider.h` named PDF in its
own description of what previewing is for.

It opens as a column of pages now, rendered by `QPdfDocument`, read-only, with the
same `Ctrl+PgUp`/`PgDn` paging the text viewer uses. Pages are rendered when a
delegate asks for one, so opening a six-hundred-page scan costs the first page rather
than six hundred, and the delegate reserves its height from the page's own aspect
first so the list does not jump about as images arrive. The rendered width is
quantised in steps because it goes into the cached file's name — bound to the raw
width, dragging a window would have re-rendered every visible page per pixel.

Two decisions were made before any code, and both are in
[ADR-0004](docs/adr/0004-pdf-previews.md). Qt PDF rather than poppler, because
poppler is GPL and does not sit with shipping Mole under Apache-2.0, while the Qt
module as packaged declares `LGPL-3 or GPL-2` — which is what the licence audit turns
on. And pages reach the screen as image files in a scratch directory rather than
through a `QQuickImageProvider`, because an image provider is registered on the
`QQmlEngine`, which a preview provider deliberately cannot reach; threading the engine
through the plugin boundary to save a temporary file would have traded a real
architectural rule for a smaller one.

`QtQuick.Pdf` would have supplied most of this view for free and was not used: its
QML module is not installed here, so depending on it would mean a second optional
dependency for one feature and a view that silently does not exist without it.
Rendering through `QPdfDocument` costs a page-image path and buys control over when
pages are rendered, which is the part that matters.

The dependency is optional. Without `Qt6::Pdf` the provider still compiles and still
refuses every file, so a PDF behaves exactly as it did before — and the test states
that both ways round, so a build without the module is a green build rather than a
skipped one.

Licence work done rather than promised: `THIRD-PARTY-NOTICES.md` records Qt Pdf and
what it embeds — PDFium and PDFium's own third-party components, all inside
`libQt6Pdf.so` rather than in Mole's binary — and the audit table in
`docs/LICENSING.md` lists the module. `make licence-check` confirms Qt is still
dynamically linked, now across twelve libraries including `libQt6Pdf.so`, and that no
GPL-only module is referenced. It also fails one check, on a stale `dist/` from an old
bundle whose launcher is still named `superfilemanager` — that failure predates this
work and is noted in TODO.md rather than quietly worked around.

The tests write their own PDF with `QPdfWriter`, because a binary fixture in the tree
is one nobody can review. They check the page count, that an A4 page comes out
upright, that asking twice at one width reuses the file while a different width
renders again, that a page past the end is nothing rather than a crash — and that the
rendered page has ink on it, since a renderer quietly producing white paper would pass
every other assertion. The walkthrough then proves the whole path in the real window:
a delegate asks, an image loads, and the strip says "Page 1 of 2".

## Bulk rename hid the thing it calls its own feature

`BulkRenameView.qml` opens by stating its own priority — *"The preview is the
feature"* — and then laid itself out as though the form were. The rules column asked
for 40% of the window and there was no minimum width anywhere in the view, so the
grids of full-width text boxes inside it stretched a two-character prefix across a
third of the screen and the before-and-after list took what was left. The form is
now capped, and the preview keeps a floor of its own, so no arrangement of rules can
crowd it out.

The other half was that nothing happened while you typed: the fields were wired to
`onEditingFinished`, so a prefix showed no effect until Enter was pressed or the
focus moved elsewhere — while the dropdowns and spin boxes in the same form updated
at once. Two behaviours in one panel, and the fields carrying the interesting part
were the ones that felt dead.

The first attempt at this was wrong and worth recording. Measuring `RenamePlan::build`
first — 2 ms for a thousand files, 15 ms for five thousand, 63 ms for twenty thousand
— it looked like live updates needed coalescing, so a debounce went in. That was
solving a problem nobody had: the complaint was about not seeing the changes, and a
debounce delays exactly the feedback being asked for. It also made the preview
asynchronous, which broke a test that reasonably expected the plan to be current. It
came out again.

The real cause was elsewhere and would have survived any amount of debouncing. Every
keystroke made `setRuleField` emit `rulesChanged()`, the form's `Repeater` rebuilt its
delegates, and the field being typed into was destroyed and replaced. Typing "2024_"
left "2": the first character round-tripped through the model, the field was
recreated, and the rest went nowhere. `setRuleField` no longer announces that the
rules changed — the form is the only thing that reads them and it is where the change
came from; what has to follow the keystroke is the preview, and `previewChanged()`
says so.

The test types into the field rather than calling the controller, and keeps the
keyboard there while it asserts, because the whole bug lived in the difference
between those two things. It also holds the layout: the preview list must keep at
least 320 pixels.

## The small controls were too small to hit

Adding a bookmark and closing a tab — the two things anyone does most — were
`ToolButton`s of 22 by 22 with a text glyph inside, and the drive's remove button was
20 by 20. Fiddly to hit, and they read as afterthoughts.

It was never two files. Fifty-two explicit `implicitWidth` or `implicitHeight` values
sat below 24 across 18 QML files, so the fix was a decision rather than a nudge:
`App.minimumTarget` is now the floor for anything that is only an icon, at 28 —
twenty-four is the figure usually quoted as a minimum for a pointer, and on a desktop
something nearer thirty stops feeling like a pinprick. Twenty-four controls were
raised to it, and nineteen glyphs now take their size from the type scale, because a
bigger button with the style's default mark in the middle looks emptier rather than
clearer.

What was left alone, deliberately. The remaining small sizes all belong to
`BusyIndicator`s, which are not click targets — a spinner does not need to be
reachable. And several of these controls appear only on hover, the drive's × among
them; that is a discoverability question rather than a size one, a drive can also be
removed from the Drives dialog, and changing when a control appears is a different
decision from how big it is when it does.

The floor is testable and the look is not, so the test holds the floor: the two
controls the request named are at least `minimumTarget` in both directions and their
glyph reports exactly `textSize`. It is two rather than all of them because a
tree-wide assertion would need every icon-only control to carry an `objectName`
first — recorded in TODO.md rather than left implied.

## The type was too small, and there was no scale to raise

A file name was 13 pixels, most of a listing 12, supporting text 11, and in places
it dropped to 10 and 9. A file manager is a thing people stare at all day.

Raising the numbers where they stood was the wrong shape of fix: there were about
270 `font.pixelSize` literals across 27 QML files, so "a bit bigger" would have been
270 edits and the next view added would have guessed its own size again. The sizes
now come from `AppController`, for the same reason the monospace family already did
— picked once so that a listing, a preview and a form line up instead of each
choosing. Five steps, each with a job: `headingSize`, `textSize` for primary content,
`secondaryTextSize` for sizes and dates and labels, `smallTextSize` as captions *and*
the floor, and `monospaceSize` for code, which reads a shade smaller than prose.
`listRowHeight` is derived from `textSize` rather than stated, so raising the text
cannot crop a row.

Applied where the reading happens: the file listing, every preview, and the sidebar
— which was not in the original plan but ended up looking small next to a listing
that had grown, and it is the next place the eye goes. Around 200 literals remain in
the other views and adopt the scale as those views are touched; that is recorded in
TODO.md rather than left as a surprise.

Constant for now, and deliberately so: when these become a preference the views do
not change, which is the whole point of them living in one place.

Two tests, because "looks better" is not assertable but two things around it are.
The scale's shape is held at the application level — the steps in order, nothing
below the floor of eleven, code no larger than prose — and the binding is held in the
real window: a listing row's name label must report exactly `textSize`, because a
literal left behind in a delegate is invisible until someone compares two views side
by side. Whether the result is *pleasant* is still what `make screenshots` and a
human are for.

## How big is this folder, answered in the listing

`Ctrl+Shift+S` measures the ticked folders — or every folder in the listing when
nothing is ticked, because "which of these is the big one" is the question — and
writes each total into its row as the walk finishes it. A background `Task` like
everything else that takes time: progress, cancellable, visible in the task strip,
and the window stays usable throughout.

No second tree-walker: `FolderSizesTask` uses the same `DirectoryWalker` the
analysis and the indexer use, so cancellation, unreadable directories and symlink
loops stay solved in one place. What it does not reuse is `AnalyseDirectoryTask`
itself, which was the first plan — it produces a whole `AnalysisReport` per folder,
and forty folders would mean forty reports built and thrown away to read one number
off each.

A measured total lives beside the entry rather than in it. `FileEntry::size` for a
directory is the inode's own size, and writing a recursive total over it would make
one field that is sometimes one thing and sometimes another — a field nobody can
trust afterwards. Keyed by uri, so re-sorting or filtering cannot move a number onto
the wrong row, and dropped whenever the listing is replaced: a measurement describes
the tree as it was when it was taken, and a stale number is worse than an empty cell.
Sorting by size uses the measured total for folders that have one, which is the only
number anyone means when they sort a listing by size.

Two things the tests had to be dragged into being honest about. Cancellation cannot
be tested on a local disk: `folderSized` is queued to the test's thread, and by the
time the cancel is sent the worker has already finished the next folder, so both
answers arrive and the test proves nothing — it needed a drive that takes its time
listing, and then it asserts exactly one whole answer arrived. And "cancelled" must
never mean "reported half a folder as a total", because a wrong number in a listing
is worse than none, so the task checks for cancellation between finishing a folder
and announcing it.

Also covered: an empty folder answers zero rather than staying silent for ever, a
folder the walk cannot fully read reports what it managed, files are counted but the
directories in between are not, the ticked folders win over the whole listing when
there are any, and a refresh clears what was measured.

## F3 did nothing on a folder

`F3` previews the file under the cursor, `currentFile()` returns nothing for a
directory, so the action was disabled and the key did nothing at all — which is
indistinguishable from a key that is broken. On a folder it now opens it, the same
thing `Return` does, through the same `openRow()` the pane already uses.

Handled in the pane rather than in the action, deliberately. The menu entry says
"Preview this file" and stays disabled on a folder, because that is what it says it
does; it is the *key* that carries the second meaning, which is how function keys
have always worked in a commander.

The test asserts which of the two paths ran, since both are one keypress and easy to
confuse: on a folder the listing navigates and no tab appears, on a file a tab
appears and the listing stays put. It holds the pane pointer from the start, because
`pane()` asks the current tab for its pane and the current tab is a preview by the
end — the first version of the test dereferenced that null and took the whole binary
down with it.

## The menu had one heap called Tools

Eleven entries under one heading, of two entirely different kinds. *Preview this
file*, *Terminal here*, *Add to set* and *Index this folder* do something to the
files in front of you and hand you back to the listing. *Analyse folder*, *Find
duplicates*, *Bulk rename*, *Sync folders*, *Saved reports*, *Alerts* and
*Scheduled jobs* open a tab that is a tool you then work in. Read as one list they
are indistinguishable, so finding anything meant reading all of it.

`Tools` is now `Operations` and `Workflows`, and the deciding question is written
down: does the entry do something to the files in front of you, or hand you a tool
to work with? The tie-break for the ones that sound like both — if it needs a tab of
its own to be useful at all, it is a workflow. *Bulk rename* is a workflow even
though it acts on a selection, because what it opens is a tool with rules and a
preview. *Add to set* is an operation even though the sets view is a workflow,
because adding the selection to a set is one act, finished when it is done.

This is an extension point, not decoration: `Section` is what a plugin picks, so
leaving it as one bucket guaranteed plugins would keep filling the same bucket. The
names, the rule and the alternatives that lost are in
[ADR-0003](docs/adr/0003-menu-sections.md) — including why not `Tasks` (the
application already shows running tasks in a strip and the word would mean two
things), why not `Selection` (*Terminal here* acts on the folder, not a selection),
and why not `Actions` (every entry in a menu is an action).

`Section::Tools` is gone rather than deprecated, which breaks any out-of-tree plugin
that named it. That is deliberate: an alias would let a plugin keep dodging the
question this change exists to force. `docs/WRITING_PLUGINS.md` documents both
sections and the default is now `Workflows`, since a contributed feature tab is the
common case.

Two tests, at both levels. The registry one proves the sections come out in a fixed
order and that entries land where they asked to; the application one proves it for
the real eleven entries rather than for a registry fed by a test. What no test can
settle is whether a given entry was *filed* correctly — that is what the rule and
its worked examples are for.

## The F4 menu stopped answering the keyboard halfway along

Opening the menu with F4 worked, and so did stepping along the headings and opening
one with Right or Enter. Coming back out was where it ended: Left closed the
submenu and left the menu it came from without the keyboard, so the arrows did
nothing from then on and the only way forward was the mouse — which is the whole
thing F4 exists to avoid.

Measured rather than assumed, and it took three attempts to get right, each one
corrected by what the previous measurement said. Restoring the focus inside the
`closed` handler does nothing, because Qt moves the focus as part of closing the
popup and takes back anything claimed there; deferring with `Qt.callLater` does
work, but `closed` only fires once the exit transition has finished, which is
around a fifth of a second in which the menu is still deaf — long enough for the
next keystroke to fall into the gap, and long enough that an early version of the
fix reset the highlight *after* the user had already moved off it. The hand-back
now happens on `aboutToHide` as well, and it only restores the highlight when Qt
has actually cleared it, so it can never undo a heading the user has just moved to.

The five submenus were five near-identical blocks, and they had already drifted:
only File declared `focus: true`. They are one `SectionMenu` component now, which
is why the behaviour cannot differ between them again — the drift was part of the
bug, not tidiness. Each submenu's `objectName` follows its section name, so a test
can address one without a second thing to keep in step.

`f4MenuWalksIntoSubmenusWithTheKeyboard` walks the whole path — open, along,
in with Right, within, out with Left, in again with Enter, out with Escape — and
was checked against the bug by putting it back. The rule this belongs to is in
[ADR-0002](docs/adr/0002-window-shortcuts-versus-focused-views.md), alongside the
terminal: focus declared is not focus held, and that applies coming back as much as
going in.

## The terminal did not get the keyboard, and Ctrl+D bookmarked instead of closing

Opening the panel left the keyboard on the file list, so the shell was on screen
while what you typed went somewhere else, and it took a click before it answered.
`focus: true` declares an intention, not a fact — something has to call
`forceActiveFocus()` when the panel is revealed, which is what the menu already
does when F4 opens it. The panel now does it too, on both paths: when it is
revealed after it exists, and when it exists only once it is already being
revealed.

`Ctrl+D` was the more interesting one. In a shell it means end of input, and the
encoding for it was already correct — `Ctrl+A`..`Ctrl+_` become control characters,
which is how `Ctrl+C` reaches the shell as an interrupt. The key simply never
arrived: it is a window `Shortcut` bound to `mole.bookmarks.add`, and Qt matches
shortcuts before offering the key to whatever has the keyboard, so the panel's own
handler — the one whose comment insists that every key goes to the shell — was
never consulted. The panel now accepts `ShortcutOverride` for everything, which is
Qt's own way for a focused item to say the key is its business.

A shell that ends should take its panel with it, which is what closing a terminal
means everywhere else, so a clean exit hides the panel. A shell that died of
something keeps it open along with the exit code, because otherwise the reason
disappears with the window.

This was the third collision of the same kind, after `F5` being swallowed by
`StandardKey.Refresh` and `Ctrl+W` being claimed by a read-only editor, so the rule
for all three is written down in
[ADR-0002](docs/adr/0002-window-shortcuts-versus-focused-views.md) rather than
being rediscovered a fourth time. Note that the terminal needed the opposite of
`ViewerKeys`: not a view handing a shortcut back to the window, but a view taking
one away from it.

Three assertions, and each was checked against its own bug by putting the bug back.
Typing after the panel opens reaches the shell — typed on the keyboard, not sent
through the controller, because every other assertion about the shell would pass
with the keyboard on the list behind it. `Ctrl+D` ends the shell, closes the panel
and adds no bookmark. And `Ctrl+D` on the listing still bookmarks the folder, which
is where that behaviour belongs.

## A slow table preview looked like a hang

Opening a large CSV showed an empty grid until the whole file had been imported,
however long that took, while the task strip insisted something was running. The
view said nothing, and a view that says nothing reads as a frozen application.

The comment in `TablePreviewController::reimport()` claimed the opposite — *"Rows
appear as they arrive rather than after the whole file"* — and it was not true. The
progress handler called `TableModel::refresh()`, but the model's source was only
attached in the `finished` handler, and `refresh()` without a source reports no
headers and no rows. So every batch of five thousand rows refreshed a model that
had nothing to look at. The store is a database that answers for whatever has been
committed to it, so the source is now attached before the import is submitted, and
the finish refreshes rather than re-sourcing — re-sourcing would clear a filter
typed while the file was still being read.

Fixing that broke a test, which was the interesting part: `parsesCsvWithADetected
Separator` had been using "there are rows" as its signal for "the import has
finished", and the detected separator was only published at the end. With rows now
arriving early, the picker above a half-filled grid was captioned with the default
guess instead of the separator actually in use. The task announces the separator
the moment the shape is settled, before the first row is stored, so the caption
tells the truth from the first row on.

What is left is the gap before the first batch, which on a slow drive is the whole
problem. The view now says it is reading, after one second, in the middle of the
grid — the threshold and the wording follow the file pane, which had solved this
already for slow folders — and gets out of the way as soon as rows land, because
rows are a better answer to "is this stuck" than any spinner.

Both halves are covered. `tableFillsWhileTheImportIsStillRunning` samples the row
count from the progress signal rather than polling, because a poll that arrives one
turn late would be looking at the finished state and pass without ever seeing the
middle. The view half needed a drive that is genuinely slow to open a file, so
`MemoryFileSystem` grew `setReadDelayMs` beside the `setListDelayMs` that the slow
folder test already used.

## Markdown previews were cramped

Qt's Markdown importer gives a heading no space above or below it, sets every
paragraph solid, and hands a fenced code block to the view as nine-point
monospace with no margins and nothing behind it. Rendered, it read as a wall of
text, which is the opposite of what a Markdown file is for.

The document is now restyled after the import: headings get room and a size that
shows the hierarchy, prose gets line spacing, code gets the application's
monospace family at a size that matches the prose and a slab behind it, quotes
keep their nesting and go quieter, tables get cell padding. The view stopped
running the text edge to edge — it keeps margins, and on a wide window the
gutters take the surplus so the line length stays readable.

Two things had to be found out by measuring rather than by reading the
documentation, and both are now written down in
[ADR-0001](docs/adr/0001-markdown-preview-typography.md): a style sheet cannot do
any of this, because `setMarkdown()` never consults one; and wrapping a code
block in a padded frame — the only thing in Qt's rich text with real padding —
injects blank lines into the document and mangles what the file says, so the rule
is formats only, never structure.

Two bugs the tests caught before they could ship. A paragraph that merely opens
with an inline `code span` is given a monospace block font by the importer, so
detecting code blocks that way handed such a paragraph a slab of its own; only
unbreakable lines are a safe signal. And the styling read a quote's nesting depth
out of the very margin it had just overwritten, which flattened every nested
quote to one level — the depth is now recorded before it goes. Applying the
styling twice is a no-op, and a test asserts it, because it runs again on every
change the document makes, including its own.

The one thing left alone is the importer itself: a blockquote and a fenced code
block each end with a stray empty block, and a table placed straight after either
one takes that block into its first cell, which loses its bold. It happens before
any of this code runs, and correcting it would mean editing the document's
structure.

## Terminal panel

A shell for the folder you are looking at, split along the bottom of the window.
Opening it starts there; navigating afterwards does not drag it along, because a
shell has its own idea of where it is and fighting that is worse than leaving it
alone. `Ctrl+\`` opens and closes it, and every other key goes to the shell — a
terminal that let the window keep `Ctrl+C` would be useless.

libvterm does the emulation when it is available, which makes full-screen
programs work properly rather than approximately; the alternate screen is enabled
so leaving an editor restores what was underneath it. Without libvterm there is a
built-in parser covering printable text, the control characters a shell relies
on, cursor movement, erasing and colour — and it says "basic mode" in the header
rather than drawing something subtly wrong.

Two things the emulator has to get right that are easy to miss, and both are
tested: an escape sequence split across two reads, and a multi-byte character
split across two reads. A read boundary falls wherever the kernel puts it.

Not available on a virtual drive, and the panel says so — there is no directory
for a process to start in inside a zip or a bucket.

## Sync

A desktop rsync in its own tab, between any two drives, because everything goes
through the VFS.

Three modes, because everybody's idea of "sync" is different: **Update** copies
what is missing or changed and never deletes, **Mirror** makes the destination
match exactly including removals, **Fill gaps** only adds what is absent. Files
are judged changed by size and time, by size alone for drives whose timestamps
cannot be trusted, or by contents when certainty is worth the reading.

The dry run is the default and Preview is the prominent button. It is not a
simulation of the real path — it *is* the real path with the last step withheld,
which is the only way a preview is worth believing. A mirror that would delete
anything asks again before it does.

Details that only show up when someone relies on them, each with a test:
timestamps get a second of slack, or every sync between two filesystems copies
everything every time; a narrow include beats a broad exclude, because
"everything except .tmp, but definitely notes" is how people express it; a
filtered-out name is never deleted by a mirror, since acting on a rule the user
did not give is worse than leaving a stray file; directories are created before
the files that go in them and deletions come last, so a cancelled mirror cannot
lose something it was about to be handed back.

## Duplicate detection

Four strategies behind one interface, expressed as ordered *stages* rather than
one comparison — because that is the shape the problem has. Every worthwhile
strategy starts with something cheap that rules most files out and only then pays
for something expensive on what is left.

"Identical contents" is size, then a hash of the first 16 kB, then a hash of the
whole file. A test with a counting strategy proves the point directly: of ten
files, ten reach the size stage and two reach the reading stage. Hashing the tree
is the obvious approach and is the difference between minutes and hours on a NAS.

The other three exist because they answer questions content comparison cannot.
"Same name" finds copies that were edited apart — where else did this file end
up — which no hash will ever pair.

Choosing what to keep is the hard half, and the tab never picks for you. It
offers the choices people actually make — keep the newest, the oldest, the copy
nearest the top of the tree — and says what each would free before anything is
deleted. Empty files are ignored: every one is identical to every other, and
listing thousands buries the results that matter.

## Bulk rename

A list of independent operations applied in order, with a live preview of every
file's before-and-after. The preview is the feature: renaming two hundred files
on faith is how people lose an evening.

Eight operations to begin with — replace (plain or pattern), case, insert,
remove, strip a character class, number, affix, extension — and the order is
meaning, not decoration: stripping digits before numbering is a different result
from numbering first, and both are legitimate. A form with eight fields could not
express either.

It refuses a batch that would collide rather than discovering it halfway: two
files taking one name, a name already taken by something outside the batch, a
name reduced to nothing, a name containing a path separator, or a file left with
only an extension. The filesystem would only notice the second collision, by
which time the first file has already moved.

Rules touch the stem by default — upper-casing a name should not turn `.txt` into
something no tool recognises — and `.gitignore` is treated as a name with no
extension rather than as an extension with no name.

## File sets

A named list of files built by hand, from anywhere, across any number of drives,
then treated as a thing in its own right.

The whole design rests on one decision: a set answers `targetUris()`, the same
question a pane's selection answers, under the same name. So bulk rename,
analysis and the rest take a set with no code of their own — a test asserts
exactly that, because it is the property that would quietly rot first. The shell
asks the current tab what it is aimed at and never asks whether that tab is a set.

A set outlives the files in it, so it can be checked: "not looked at yet" and
"not there" are distinct states, because reporting a healthy set as broken before
anything had looked would be worse than saying nothing.

## SQLite and Parquet previews

Two more viewers, both landing on a grid, and neither of them importing anything.

A SQLite file is already a queryable table, so paging and filtering are queries
against the file itself — a database of any size opens at once. It is opened
read-only through SQLite's URI form, the only way it will refuse writes outright:
previewing a file is not a licence to modify it, and a database another process
has open is exactly where that goes wrong. `immutable` is deliberately not set,
because that would promise the file cannot change while another process may well
be writing to it.

Parquet is columnar and stores rows in groups, so a window only decodes the
groups it touches. Filtering it does mean scanning — there is no query engine
behind the format — so the scan is bounded and the view says so rather than
letting an incomplete count look authoritative.

The work was mostly not the readers. The grid was written against the CSV
importer, so it grew an `ITableSource` interface and moved into `DataGrid.qml`;
selection, copying, column sizing and the filter are now shared by all three
viewers rather than existing in three drifting copies.

Arrow is optional. When it is absent `ParquetTable::isSupported()` is false, the
provider declines the file and it falls through to the information viewer — a
missing optional library must never stop the application being built. Arrow's
headers have to be included before any Qt header, because Arrow declares a
parameter named `signals` and Qt's macro of that name expands to `public:`.

## Permissions of the current folder

Beside the report and index tags, what the current user may do here.

Modelled as questions — may I read, write, add files here, delete this — rather
than as mode bits, because POSIX mode bits do not describe a Windows ACL and
neither describes a bucket policy. `Unknown` is a first-class answer: a drive
with no idea says so and the interface shows nothing, rather than a guess
presented as fact. The native form is offered alongside where the platform has
one, which on Linux is the nine characters everybody reads.

Optional on `IFileSystem` like `space()`, and the conformance suite now checks
the contract from both sides: a backend advertising the capability must answer,
and one that does not must refuse rather than return something empty.

Removing an entry is governed by the parent directory, not the entry — a
read-only file in a writable folder can still be deleted, and reporting otherwise
would be wrong in the direction that matters.

## Tasks report whatever their work is about

Progress was counted in files, which is useless for the case a progress bar
exists for: one 4 GB file sat at 0% and then jumped to 100%. It is counted in
bytes now, with throughput measured over a short window rather than over the
whole run, so a stall shows up instead of being averaged away by a fast start.

More importantly the mechanism is general. A task publishes named metrics — a
key, a label, a value and a kind (count, bytes, rate, duration, text) — and the
strip lays out whatever it finds. Sync, duplicate detection and bulk rename will
each have something different worth watching, and none of them should require the
interface to learn new vocabulary. Bytes and speed are simply the first two
users, published through a convenience that also drives the percentage.

Every task also carries when it started and how long it has been going, frozen
once it ends. An elapsed time that keeps counting after the task finished is not
a measurement of anything.

A cancelled task no longer animates. Progress of -1 means "unknown", which is
right while running and wrong afterwards — a cancelled scan was left with a bar
sweeping for ever, as though the work were still going.

## Copy and move ask the right questions first

The confirmation now shows how much is going where, offers a different name for a
single item, and names the files that already exist at the destination *before*
anything happens. Collisions come from the listing the other pane has already
loaded, so the warning is on screen the instant the dialog opens — the only
moment it is useful.

The conflict choice is explicit: stop and report, skip that file, or overwrite.
Stop is the default, because a prompt whose safe answer is not the default is a
prompt that will one day overwrite something by reflex. Choosing overwrite says
plainly that there is no undo.

## Opening a report is not a rescan

`setTargets` always walked the tree, so looking at yesterday's numbers cost a
full scan — minutes on a large folder. It loads what is saved now and walks only
a folder that has nothing saved, because an empty tab would be useless. "Analyse
folder" is a separate method that always walks, which is what asking for it
means.

## Smaller things

- **Ctrl+G showed a clipped path field.** The crumbs and the editable path share
  one slot, and the slot had a hard-coded height of 30 while a Material text
  field wants 40 — so the field appeared with its text and underline cut off,
  which reads as being covered rather than as being too small. The slot is
  measured from the field now, and from the field rather than per mode, or the
  bar would change height as the keyboard moved into it.
- **The waiting view is centred.** Same fault as the empty window: a
  `ColumnLayout` is only as wide as its widest child, so the message sat against
  the left edge of the pane. Now tested against a drive that is genuinely slow,
  which also covers the one-second threshold for the first time.
- **The mouse no longer highlights rows.** Two highlights competed for one
  meaning — the cursor is where Enter will act, and a second one trailing the
  pointer made it ambiguous which row that was.

## F5 did not copy anything

Two causes, both of which the test suite was blind to.

The first was mine, from converting `canTransfer` to a property: `BrowserView`
still called it as a method in one place. Calling a bool as a function throws,
and the throw took the rest of the handler with it — so F5 opened no dialog,
reported no error and did nothing at all.

The second was older. `StandardKey.Refresh` is `F5` as well as `Ctrl+R` on this
platform, so the window shortcut consumed F5 before the pane ever saw it. Refresh
is `Ctrl+R` only now; F5 is the commander copy key and the window has no business
taking it.

Both survived a green suite because every existing test called
`copyToOtherPane()` on the controller. The walkthrough now presses F5, accepts
the real dialog and waits for the file to appear on the other side — the only
shape of test that could have caught either fault. Finding the dialog needed a
harness addition: a `Dialog` is a `Popup`, absent from the visual tree, and
`QObject::findChild` on the window finds nothing because QML does not parent
items into the window's QObject tree. `object()` searches both hierarchies.

## Background work you cannot miss

The strip reported a count, which read as decoration. While anything is running
it is now tinted, ruled in the accent colour, and carries the running task's
name, a real progress bar and its status line — collapsed, without expanding
anything.

Finished rows retire themselves after an hour. `Task` is stamped when it reaches
a terminal state and the manager sweeps every minute. A list nobody prunes grows
for the whole session, and by the end the one failure worth seeing is buried in
it. Work still running is never swept, however stale the list.

`Hidden` moved from the status line to the toolbar, beside copy and move: it
changes what the pane shows, which is what that strip is for. It applies to both
panes, so a dual view cannot hold two different ideas of what is in one tree.

## The browser toolbar says what is already known

`Copy` and `Move` never enabled in dual pane. They were bound to
`controller.canTransfer()` — an invokable, so there was no change signal and QML
evaluated the binding once. Switching to Dual satisfied the condition with
nothing to notice it. It is a `Q_PROPERTY` with a notify signal now, fed by every
input the answer depends on: the mode, the selection and whether the far side is
writable.

The `Index folder` button is gone; indexing is a once-in-a-while action and lives
in the menu. In its place the strip carries what the application already knows
about the folder: whether it has a report (clickable — it opens the saved one
rather than rescanning), whether an alert is watching it and whether that alert
has tripped, and whether it has been indexed and when.

The index is asked about the volume the folder sits *under*, not the folder
itself: scanning `/data` indexes `/data/projects` too, and claiming otherwise
would send the user to re-scan what is already there.

## Report and alert tags on the listing

The same facts per row, beside the date. Affordability was the whole design
question: a store lookup per row would make a listing of five thousand entries
pay thousands of file opens for two small tags. Instead the report store hands
over its stored folder names in one directory read, and each row is a hash and a
set lookup with no I/O at all.

## Reports library

A tab listing every saved report — folders on the left, that folder's runs on the
right, with what each run changed by against the one before it. Sorted by most
recent activity rather than by name, because a library sorted alphabetically
makes you hunt for the one thing that moved.

The store moved out of the analysis feature and into the host, alongside the
schedule and the alerts. Three things now need it — the library, the browser
strip and alerts reading the latest report — and a store owned by one tab is a
store the others cannot see.

## Clickable breadcrumbs

`/mnt/nas/projekty` is now `/ › mnt › nas › projekty`, each piece a target.
Pressing Backspace once per level was work the interface could do. Typing is
still there on Ctrl+G or a click past the last crumb, because a pasted path has
to go somewhere. A long path scrolls to keep the end in view rather than the
start, which you already know.

## Going back restores the cursor

Navigation left the cursor at the top of every listing, so walking a tree meant
restarting at each level. The pane now remembers where the cursor stood in each
folder, and stepping up lands on the folder just left. Bounded to a few hundred
folders: the convenience is not worth an entry per folder ever visited.

An entry that has since been deleted falls back to the first row — landing on a
stale index would be worse than landing on the top.

## Ctrl+W in a preview

Clicking into a preview stopped `Ctrl+W` from closing the tab. Measured rather
than guessed: a `Keys` handler on the text area showed the key arriving there,
which meant the read-only `TextArea` had accepted the shortcut-override event and
Qt had skipped the matching `Shortcut`. `Ctrl+W` is `DeleteStartOfWord` in the
standard editing bindings, and the control then discarded it because the document
is read-only.

Qt offers no declarative way to un-claim those keys, so `ViewerKeys.qml` hands
them back to the window — one relay rather than a private copy of the shortcut
table per view, and narrow enough to leave the keys a viewer genuinely uses.

Two further defects surfaced while doing it. `attachHighlighter` was called on
every text change, and attaching rehighlights, which changes the text: infinite
recursion, reported only as a stack-overflow warning nobody had read. And the
final window of a large file was unreachable — snapping the window's start back
to a line boundary shortened it, so "is there more after this?" was always yes.

## Alerts

A tab that lists what is being watched, what tripped, and a form for watching
one more. Eleven metrics — total size, free space in bytes and per cent, file
and folder counts, largest file, hours since anything changed, permissions, last
modified, existence, unreadable folders — compared with above / below / changed
/ equals.

Two design points worth keeping:

- A metric that could not be read is `Failed`, never `Ok`. An unreachable drive
  reported as a green tick is the worst outcome available, because it looks
  exactly like everything being fine.
- `Changed` treats its first reading as a baseline rather than firing. Otherwise
  every alert would trip the moment it was created, which teaches the user to
  ignore the first one they ever set.

An alert can read from the latest saved report instead of measuring live —
instant, and only as fresh as that report, which is what scheduling the report
is for. It deliberately does not fall back to a live walk when no report exists;
that would quietly turn it into a different alert with different timing.

## Delimited files with no row limit

The CSV/TSV viewer stopped at 5000 rows and filtered only what it had loaded.
Now the file is streamed into a scratch SQLite database and every question is a
query, so paging and filtering cover the whole file however large it is.

- `DelimitedStreamParser` parses a chunk at a time and carries the state a row
  straddling a chunk boundary needs, including a quoted field with newlines in
  it.
- Columns are measured from the contents during the import, so the grid fits
  what is in it instead of a default that wasted half the window.
- Cells select by click, shift-click and drag, and copy as tab-separated text —
  what every spreadsheet expects on paste.
- The table widens to the widest row in the sample rather than to the header
  alone: a header that does not mention every column is common, and sizing to it
  would silently drop the extra fields.

## Text preview of very large files

The viewer held the whole file. Now it holds only the window being shown, read
through a seek, so a 100 GB log opens as fast as a 100 byte one. Windows snap to
line boundaries at both ends, or paging would show a severed line at every step.

Alongside it: source highlighting for twenty-odd languages from a table rather
than hand-written code per language, Markdown rendered instead of coloured, and
one monospace family chosen once by the application so every code and data view
lines up.

## Drive capacity in the sidebar

Each drive shows how full it is — amber past three quarters, red past nine
tenths — with free and total beside it. Drives that have no meaningful capacity
show only a name: a bucket has no size in any useful sense, and a chart is read
as a fact.

`IFileSystem::space()` is optional and defaults to `NotSupported`; only backends
advertising `ReportsSpace` are asked. The query goes through a task like
everything else that touches storage, because `QStorageInfo` blocks on an
unreachable NFS mount and the UI thread must never wait on a disk.

## Automation

Reports can be put on a clock. `Scheduler` polls rather than arming a timer per
rule, because a laptop asleep for two days has to notice on waking. A rule that
has never run is due immediately, so a job whose turn came while the application
was closed runs at the next start instead of waiting out another interval.

The tracking tab sorts broken rules first, counts consecutive failures, and
records three things the design deliberately makes visible: a rule whose plugin
is gone is `Skipped` with the reason, a run interrupted by a quit comes back as
`Failed` rather than stuck at `Running`, and every attempt is logged whether or
not it fired.

## A test harness that does not lie

The Xvfb-and-`xdotool` setup was worse than no harness: with no window manager
there is no X input focus, so roughly two of every six synthetic keystrokes
arrived and tests went green because nothing happened. It cost four rounds on
one Enter-key bug — a real defect that looked like a harness fault, next to a
harness fault that looked like a real defect.

`QmlAppHarness` builds the whole application offscreen and posts keys straight
to the `QQuickWindow`: the same delivery path production uses, minus the
display. Screenshots come from `QQuickWindow::grabWindow()`, so `make
screenshots` cannot produce a picture of a state the assertions did not just
verify.

## Smaller things

- **Closing a tab returns to the one it was opened from.** Each tab remembers
  its opener; position alone would send you to whichever tab happened to sit
  next to it, somewhere you were never working.
- **A working tab says so.** `FeatureController::busy` now reaches the tab strip
  as a spinner, so a report still running on a large tree does not look like one
  that finished.
- **The filter keeps the keyboard while narrowing.** Enter opened nothing and an
  arrow moved focus instead of the cursor; both cost a keystroke and swallowed
  the one just spent. Fixing it surfaced a second defect: navigating with a
  filter active left the new folder silently filtered by the old term, with
  nothing on screen to explain the missing files.
- **The empty window is centred.** A `ColumnLayout` is only as wide as its
  widest child, so centring inside it put the block off to one side of a wide
  window; and the tab stack kept claiming half the height until it was hidden
  rather than merely emptied.
- **The empty window offers only what it can open.** `IFeature::needsContext()`
  marks a tab that is meaningless without a selection — a preview needs a file,
  a report needs a folder — and those are left out. A button that opens an empty
  tab reads as broken rather than as inapplicable.
- **Combo boxes size to their widest entry.** The repeat picker had a fixed
  width and truncated its own labels.

## Adding a drive did nothing

Reported: pressing "+" under "Your drives" had no visible effect. Three separate
faults sat on top of each other, and each on its own was enough to produce
exactly that symptom.

**The form had no size.** The field area was a `ScrollView` whose content was
sized to the view. A `ScrollView` takes its own implicit size from its content,
so that closes a loop: width decides whether a scrollbar is needed, the
scrollbar decides the available width, the available width decides the width.
Qt detects the loop, prints `Polish loop detected. Aborting after two
iterations.` and abandons the layout — which leaves every child present,
visible in the tree, and zero pixels tall. It is now a `Flickable`, which takes
its size from the layout that placed it and is told its content size, so
nothing feeds back.

**The list could not update.** `model: App.configuredDrives()` and
`model: App.driveKinds()` bound to method calls. A method call in a QML binding
has no change signal, so it is evaluated once and never again — a saved drive
would never appear in the list beside the form however well the save worked.
Both are now `Q_PROPERTY` with `NOTIFY drivesChanged`, emitted from save,
remove, connect, disconnect and unlock. This is the third time this session
this exact defect has appeared, after `canTransfer()` and `rowSpans()`.

**Pressing add changed nothing.** The dialog already opened in the blank
"new drive" state, so a button that reset it left the screen exactly as it was.
`beginAdding()` now opens the kind picker as well, and the empty right-hand
panel says what to do instead of sitting blank. The reset stayed a separate
function because saving also resets, and a save should not throw a
sixty-backend dropdown open in the user's face.

Also capped that dropdown's height: at full size sixty entries covered the
dialog, the window behind it and the drive list being chosen for.

The lesson is the one already recorded above under the F5 bug, restated: every
existing drives test called `saveDrive()` on the controller, and the controller
was fine. Only a test that presses the button can see a button that does
nothing. `theDrivesDialogOffersBackendsAndAForm` now clicks "+", picks a kind,
asserts the form is on screen *with a size*, saves, and waits for the drive to
appear in the list.

## A log for the runs that end badly

`make run` now keeps a session log, because a crash takes the terminal's
scrollback with it and the useful lines are the ones printed just before the
fall.

Every line is flushed to the file and then to the operating system as it is
written. A buffered log loses exactly the part worth reading. The previous run
is kept beside the current one as `session.log.1`, because the way anybody
reacts to a crash is to start the program again, and that restart is what would
otherwise destroy the evidence.

When the program does fall over, a signal handler writes the backtrace into the
same file and then lets the process die exactly as it would have -- default
handler restored, signal re-raised -- so the shell still reports a crash and any
core pattern still gets its core. The handler allocates nothing, locks nothing
and calls no Qt: all three deadlock or fault a second time in a process that is
already broken. It writes with `write()` and `backtrace_symbols_fd()` to a
descriptor captured when logging started.

The tests crash on purpose, in a forked child, and the parent reads what the
child managed to write on its way down. That covers the part that is easy to
get wrong and impossible to notice: `tst_SessionLog` caught the signal number
being written as "06" instead of "6".

## Two type errors in the drives form

Reported alongside the above: the form appeared but printed
`Unable to assign QJSValue to QString` once per field.

Field defaults come from each backend's own metadata, so they arrive as
numbers, booleans, nulls and lists as well as text -- and a text field handed a
list refuses the whole binding, leaving the field blank. `fieldValue()` now
returns a string whatever it is given. A second warning with the same root,
`Could not convert array value at position 0 from QString to QChar`, went with
it: that is what assigning a JS array to a string property looks like.

The test that covers it builds a form for every one of the 59 backends and
fails on any warning at all, rather than leaving them to be noticed in a
console. 707 fields, silent.

## The segmentation fault: a layout re-entering itself

Reproduced, with a stack, and fixed.

The log's own backtrace stopped after two frames -- rclone brings a Go runtime
into the process, Go forwards signals to handlers installed before it started,
and no in-process unwinder can walk past that trampoline. So the application was
driven under gdb on a virtual X server with xdotool, through the reported steps:
open Drives, set a passphrase, then switch backends in the dropdown. It fell over
on the fifth switch, and gdb had the whole stack:

    qmlAttachedPropertiesObject
    QGridLayoutItem::stretchFactor
    QGridLayoutEngine::setGeometries
    QQuickGridLayoutBase::rearrange
    QQuickLayout::geometryChange
    QQuickItem::setWidth
    QQuickFlickable::geometryChange      <- the scroller
    QQuickGridLayoutBase::rearrange      <- already rearranging
    QQuickLayout::updatePolish

A layout inside a scroller, with its width bound back to that scroller,
re-enters itself: the outer layout sizes the scroller, the column's width
binding fires, and the column rearranges while the outer rearrange is still on
the stack. Switching backends destroys every field while that is happening, and
the layout engine reads the attached `Layout` properties of an item already on
its way out. That read is the crash.

It is the same circularity that produced `Polish loop detected` earlier in this
file. Fixing the warning did not fix the loop -- it moved it somewhere Qt could
not detect it, where instead of giving up it recursed until it touched a dead
object. The fields are now a `ListView`: a view owns its delegates, expects its
model to change under it, and never drives the layout that placed it.

Two things the harness could not see, and one it now can. It could not see the
crash: offscreen and even on a real X server, driving the same properties from
C++ never produced the polish cycle that a rendered window does. It could not
see that the dropdown had stopped opening at all -- capping `popup.height`
against `popup.implicitHeight` collapsed it to nothing, because that height is
zero while the popup is shut. That one is now a test: open the dropdown and
assert it is between 100 and 340 pixels tall, since "opens" and "is visible" are
not the same claim. The form test also now asserts the fields have a width,
which is what a layout the engine has abandoned does not give them.
