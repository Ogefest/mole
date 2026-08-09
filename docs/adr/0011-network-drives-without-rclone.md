# ADR-0011: Network drives are four hand-written engines in a plugin, not rclone

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

Cloud and network drives were rclone. That bought a great deal at once — forty-odd
providers, every field of every one of them described by rclone itself at run time,
so this codebase never had to know what S3 needs and could not be wrong about it.

Two years of that trade turned out to be a bad one.

**The size.** `librclone.so` is 115 MB of Go. It was never vendored; `make librclone`
built it from source, which is why the repository could pretend the weight was not
there. It was there for anyone who wanted the feature.

**The configuration.** `RcloneFactory::variants()` generated a form from rclone's
own option registry. That is exactly why `ConnectionField` grew `advanced`, and
`dependsOnKey`/`dependsOnValues`: a provider can have eighty options, S3 asks
different questions for AWS than for Ceph, and without somewhere to hide the long
tail the dialog is unusable. The machinery worked and the result was still a form
nobody could fill in without guessing. Breadth nobody asked for, bought with a
dialog nobody could use.

**It was never actually a plugin.** Despite living in `src/plugins/rclone/`, it was
compiled statically into `mole_builtin` and registered from `BuiltinPlugin.cpp`.
The one shipped feature on the real loadable path was the archive plugin.

There was no earlier ADR for any of this; the rclone seam predates the practice.

## Decision

**rclone goes, entirely** — the sources, `scripts/build-librclone.sh`, the Makefile
target, `MOLE_LIBRCLONE_PATH` and `MOLE_LIBRCLONE_BUILD_PATH`, the 115 MB in
`third_party/`, and the connection-string design that existed only to serve it.

**Four engines, written here: SFTP, FTP, S3 and WebDAV.** Each is a hand-written
backend with a short `connectionFields()` list, so the form asks what the protocol
needs and nothing else. S3 takes an endpoint and addressing style as ordinary
fields, which is what makes one engine serve AWS, Backblaze B2, MinIO, Ceph,
Wasabi and R2 rather than needing a variant each.

**They ship as one genuinely loadable plugin**, `mole_plugin_network` — built with
`qt_add_plugin` against `mole_sdk`, publishing itself through
`IPlugin::registerExtensions()`, exactly as `ArchivePlugin` does. It is the first
of a series: further backends arrive as further plugins, not as more code in the
core.

**libcurl is the single transport dependency**, with OpenSSL — already required for
the credential store — for S3 request signing. Nothing else is taken on.

**SigV4 is implemented here**, about a hundred and fifty lines of HMAC-SHA256 chain,
checked against AWS's own published test vectors.

**There is no SSHFS engine, and no FUSE anywhere.** A Mole drive is a virtual drive
inside the application, reached through `IFileSystem`; it is never a mount the rest
of the operating system can see. SFTP covers talking to an SSH server, which is what
was actually wanted.

## Reason

### Why libcurl and not the alternatives

**It matches the threading contract exactly.** `IFileSystem` is documented as
synchronous, called only from TaskManager worker threads, so backends can be
"written in plain blocking style with no async plumbing". libcurl's easy interface
is blocking. Qt's `QNetworkAccessManager` is asynchronous and needs a running event
loop, so every backend call would have to stand up a loop on a worker thread and
pump it — async plumbing in the one place the architecture went out of its way to
forbid it. That, more than any feature list, is why Qt Network lost.

**One library covers all four.** The libcurl here speaks `sftp`, `ftp`, `ftps`,
`http` and `https`. The alternative was libssh2 for SFTP plus a hand-rolled FTP
client plus an HTTP client — three things to get right, and libssh2's headers are
not even installed on this machine, while libcurl's are.

**It is not ballast.** libcurl is about a megabyte, is already present on every
target system, and is the most heavily reviewed transport library in existence.
Replacing 115 MB with 1 MB is the entire point of this record; replacing it with
something that had to be built from source would not have been.

**aws-sdk-cpp was rejected for repeating the mistake.** It is tens of megabytes,
brings its own HTTP stack and its own build system, and would have made S3 the
heaviest thing in the tree — trading a Go blob for a C++ one. What we needed from
it was a signature algorithm, and that is a page of code.

### Why the config is hand-written

Generated forms were the root of the complaint. A hand-written `connectionFields()`
list cannot ask eighty questions, because someone has to type all eighty. The
constraint is the feature. `advanced` and `dependsOnKey` stay in the interface —
S3's addressing style is a genuine either/or, and a port number is genuinely
advanced — but they are now used where they help rather than to make generated
breadth survivable.

Connection strings go with rclone. `:sftp,host=…,user=…:` was rclone's addressing
scheme, adopted because rclone's config file stores passwords in a reversible
obfuscation it calls "obscure" and nothing was ever going to be written there. A
hand-written backend takes its config straight from `create()`, and `Password`
fields already arrive refilled from the encrypted store by `RemoteRegistry`, so a
backend never learns where its password lives.

### Why no FUSE

SSHFS was on the original list and is not here. It is SFTP over FUSE: as an engine
it would add exactly one capability over the SFTP backend — a mount the rest of the
system can see — and would pay for it with an external `sshfs` binary, a mount
lifecycle to supervise, and stale mounts to clean up after a crash.

The deciding argument is not cost but portability. FUSE is a Unix mechanism.
Mole intends to run on Windows, and a design where remote files are only reachable
through a kernel mount cannot follow. Keeping every drive virtual and
in-application means one code path on all three platforms, and it is the design the
VFS layer already had.

## Consequences

- **Google Drive, Dropbox, OneDrive and Mega are gone**, and do not come back
  without a plugin each. They speak proprietary APIs no protocol here reaches. This
  is the accepted price of the decision, recorded so it is not later mistaken for a
  regression.
- **Backblaze B2 survives as S3**, through its S3-compatible endpoint, as do MinIO,
  Ceph, Wasabi and R2. Nextcloud and ownCloud arrive as WebDAV. Between them the
  four engines reach the significant majority of places worth connecting to.
- **A drive configured against rclone stops working.** Its entry stays in
  `remotes.json` but no factory claims the scheme, so it is reported as
  unavailable rather than silently disappearing. There is no automatic migration:
  the settings rclone wanted and the settings an SFTP or S3 backend wants are not
  the same shape, and inventing a translation would produce drives that look
  configured and cannot connect.
- **A curl easy handle is not thread-safe**, and `IFileSystem` must tolerate
  concurrent calls. Each backend therefore keeps a small pool of handles and lends
  one out per call, which both satisfies the contract and preserves libcurl's
  connection reuse — an SFTP drive does not renegotiate SSH for every listing.
- **SFTP host keys are verified against `~/.ssh/known_hosts`.** An unknown host is
  accepted on first connect and remembered; a host whose key has *changed* is
  refused. That is the case that matters, and it is refused rather than reported as
  a connection failure so the reason is visible.
- **Without libcurl the plugin is not built**, and network drives are absent from
  the list rather than offered and failing — the same pattern as libarchive for
  archives and Qt PDF for previews, reported by the same kind of configure-time
  message.
- **Each engine answers to the conformance suite**, which was written for exactly
  this moment: a backend's test file builds a context and calls
  `runFileSystemConformance()`, so a new backend cannot disagree with local disk
  about what `NotFound` means. SigV4 is additionally checked against AWS's
  published vectors, offline, because a signing bug is otherwise indistinguishable
  from bad credentials.
- **Live tests skip when no credentials are in the environment.** They are driven
  by `MOLE_TEST_*` variables and never carry an account of their own, so the suite
  stays green on a machine that has none and no secret is ever committed.
