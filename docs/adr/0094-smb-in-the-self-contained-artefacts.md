# ADR-0094: SMB is not built into the self-contained artefacts

- **Date:** 2026-09-04
- **Status:** Accepted

## Context

Windows-share support goes through **libsmbclient**, which is Samba, which is
**GPL-3.0-or-later**. Mole is Apache-2.0, and the network plugin that links it is
Apache-2.0 too. The library is found at configure time by
`pkg_check_modules(SMBCLIENT …)` and linked into `mole_network_backends`, which
is compiled into the one network plugin that also carries SFTP, FTP, S3, WebDAV
and NFS.

Whether that is a problem depends entirely on how the two travel:

- In a **distribution package** — a `.deb`, an `.rpm`, a Flatpak, or a build
  somebody makes on their own machine — libsmbclient comes from the distribution.
  This project redistributes nothing of Samba, and the distribution has already
  met Samba's terms. There is no question to answer.
- In a **self-contained artefact** — the AppImage and the `make bundle` tarball —
  the bundler copies the build machine's libraries in beside the application, and
  the single file that results is what somebody downloads. That is the case where
  an Apache-2.0 program and a GPL-3 library are handed over together as one thing.

This is the same shape as [MOLE-322], which found v0.1.0 going out with a
distribution's ffmpeg closure inside it — libx264, libx265, libxvidcore and
libzvbi, all GPL-2+, and libfdk-aac, which is not free software at all. The
answer there was that the artefact carries none of that stack and video is left
to the ffmpeg on the user's own machine.

The paperwork made it worse rather than better: until [MOLE-355],
`THIRD-PARTY-NOTICES.md` had no row for libsmbclient at all, and said the network
plugin's only dependency was curl.

## Decision

**The self-contained artefacts are built without SMB. Everything else keeps it.**

`MOLE_WITH_SMB` is a CMake option, on by default. `scripts/package-appimage.sh`
and `make bundle` configure with it `OFF`; nothing else does. So:

| Artefact | Windows shares |
|---|---|
| `.deb`, `.rpm`, Flatpak, `make install` | yes — libsmbclient comes from the distribution |
| AppImage, `make bundle` tarball | no |

Two guards keep it that way. `package-appimage.sh` installs
`libsmbclient-devel` for the rest of the build and then asserts the configure log
says *Windows shares: not built* — an absence stated out loud, because a build
that quietly stopped passing the flag would find the library and ship it.
`make-bundle.sh` refuses outright to assemble a bundle whose network plugin still
has a `NEEDED` entry for libsmbclient, naming the option to turn off: a bundle is
assembled by whoever runs the script and a licence question is not theirs to get
wrong.

## Reason

Three options were weighed and the author chose the first.

**Drop it from the self-contained artefacts.** Taken. It is the call this project
has already made once, for the same reason, about a larger and more useful
feature: a self-contained artefact carries only what Apache-2.0 can absorb, and
what it carries is decided here rather than by a build machine's packaging. The
cost is that `smb://` does not work in the AppImage or the tarball. That cost is
smaller than it looks — SMB is the one backend whose users are overwhelmingly on
a machine that already has Samba installed, and the two distribution packages,
which are the recommended way to get Mole, keep it.

**Ship it with the notice.** Rejected. The position would be that the plugin and
the library are separate works communicating through a documented C API, and that
putting them in one archive is mere aggregation. It is an arguable position and it
is not a settled one; MOLE-322 declined to take the equivalent position about the
codecs, and taking it here would leave the project defending in one artefact what
it refused in another.

**Split the network plugin so SMB is its own.** Not chosen, though it would give
the best of both. It is a real change to a backend that works, for a licence
reason rather than a functional one, and the build switch gets the same outcome
today. If somebody later wants SMB in the AppImage badly enough, this is the route
and it can be reopened then.

**Why a build switch rather than a line in the bundler's exclusion list**, which
is how the codec stack is handled: libsmbclient is linked into the network plugin
rather than loaded by it. Leaving the `.so` out of the bundle would not disable
SMB — it would make the whole plugin fail to load, taking SFTP, FTP, S3, WebDAV
and NFS with it. The exclusion mechanism works for the codecs because nothing Mole
ships links them directly.

## Consequences

- Someone running the AppImage or the tarball has no Windows shares, and
  `mole --plugins` does not list SMB. This has to be said in the release notes and
  in the README beside the ffmpeg note, which says the same kind of thing.
- There is now a second build configuration to keep working. It is one CMake
  option and one `#ifdef`-free code path — the sources are simply not compiled —
  and both artefacts assert their own answer, so the configuration failing is a
  failed release rather than a silent one.
- `licenses/GPL-3.0.txt` ships anyway, and the notices carry the libsmbclient
  row anyway, because a `.deb` built from this source does link it and a reader
  of the source is entitled to know what it can link.
- If Samba ever relicenses, or if the split above is done, this record is
  superseded rather than edited.

[MOLE-322]: ../../CHANGELOG.md
[MOLE-355]: ../../THIRD-PARTY-NOTICES.md
