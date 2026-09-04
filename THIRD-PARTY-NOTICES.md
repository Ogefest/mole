# Third-party notices

Mole itself is licensed under Apache-2.0 (see `LICENSE`). It builds
on the components below, none of which are part of this project.

## Linked at build time

One row per library the build links, with the licence text that has to travel
with any artefact carrying it. `scripts/licence-check.sh` reads the
`licenses/…` paths out of this table and refuses an artefact that is missing
one, and it refuses a build whose CMake files name a library this table does
not — so a dependency added without a row here fails the release rather than
shipping unnamed. See MOLE-355.

Every licence below was confirmed against the packaged library's own copyright
file, not from memory. Several carry permissive third-party parts as well;
where that changes what has to be shipped, the row says so.

| Component | Licence | Text | How it is used |
|---|---|---|---|
| **Qt 6** (Core, Gui, Network, Qml, Quick, QuickControls2, Sql, Concurrent) | LGPL-3.0 | `licenses/LGPL-3.0.txt` | Dynamically linked |
| **Qt 6 Pdf** | LGPL-3.0 | `licenses/LGPL-3.0.txt` | Dynamically linked when present; renders PDF previews. Embeds **PDFium** (BSD-3-Clause, Copyright The Chromium Authors) and PDFium's own third-party components — FreeType, libpng, libtiff, lcms, OpenJPEG and others, under BSD, Apache-2.0 and similar permissive terms. All of it lives inside `libQt6Pdf.so`, not in Mole's binary |
| **Qt 6 Multimedia** | LGPL-3.0 | `licenses/LGPL-3.0.txt` | Dynamically linked when present; plays audio and video previews through whatever backend the platform Qt was built with |
| **Qt Test** | LGPL-3.0 | `licenses/LGPL-3.0.txt` | Test binaries only; never shipped in a release |
| **libarchive** | BSD-2-Clause | `licenses/BSD-2-Clause.txt` | Dynamically linked by the archive plugin |
| **libcurl** | curl licence (MIT/X-style) | `licenses/curl.txt` | Dynamically linked by the network plugin; carries SFTP, FTP/FTPS, S3 and WebDAV. Uses whatever SSH and TLS libraries the system's curl was built against |
| **libsmbclient** (Samba) | GPL-3.0-or-later | `licenses/GPL-3.0.txt` | Dynamically linked by the network plugin; Windows shares. **The one row with a condition attached** — see *SMB and the self-contained artefacts* below |
| **libnfs** | LGPL-2.1-or-later, with BSD-3-Clause parts | `licenses/LGPL-2.1.txt`, `licenses/BSD-3-Clause.txt` | Dynamically linked by the network plugin; NFS exports |
| **libgit2** | GPL-2.0 with the linking exception | `licenses/GPL-2.0-with-linking-exception.txt`, `licenses/GPL-2.0.txt` | Dynamically linked; reads a checkout's state for the repository band. The exception is what makes linking it from an Apache-2.0 application possible; it is quoted in full in the text file |
| **libvterm** | MIT | `licenses/MIT.txt` | Dynamically linked when present; the terminal panel's escape-sequence parser |
| **Apache Arrow** and **Apache Parquet** | Apache-2.0, with BSD-3-Clause parts | `licenses/Apache-2.0.txt`, `licenses/BSD-3-Clause.txt` | Dynamically linked when present; reads a Parquet file for the table preview |
| **xxHash** | BSD-2-Clause | `licenses/BSD-2-Clause.txt` | Dynamically linked when present; digests the head of a candidate file in a duplicate scan, which is a filter and not a proof. Optional — without it the filter falls back to SHA-256 |
| **OpenSSL** | Apache-2.0 | `licenses/Apache-2.0.txt` | Dynamically linked; AES-256-GCM for the credential store and HMAC-SHA256 for S3 request signing |
| **SQLite** | Public domain | — | Reached through Qt's `QSQLITE` driver, not linked directly |

Qt is used **unmodified**. No Qt source file is copied into this repository.

## SMB and the self-contained artefacts

`libsmbclient` is **GPL-3.0-or-later**, and the network plugin — which is
Apache-2.0 — links it dynamically for Windows-share support. Dynamic linking
from a differently-licensed program is the ordinary GPL question, and the
answer differs depending on how the two travel together:

- In a **distribution package** (`.deb`, `.rpm`, Flatpak) the library comes
  from the distribution and is not redistributed by this project at all. There
  is nothing to reconcile.
- In a **self-contained artefact** — the AppImage and `make bundle` output —
  the library is copied in beside the application, and the combined artefact is
  what is handed to somebody. That is the case the decision below is about.

See [ADR-0094](docs/adr/0094-smb-in-the-self-contained-artefacts.md) for the
options that were weighed and which one was taken.

## Additionally present in `make bundle` output

The self-contained bundle copies whatever the build machine's Qt depends on.
On a typical Linux system that is roughly 120 shared libraries, including:

| Component | Licence | Text |
|---|---|---|
| glibc, libstdc++ (**not** bundled — taken from the host) | LGPL-2.1 / GPL-3 with exception | `licenses/LGPL-2.1.txt`, `licenses/GPL-3.0.txt` |
| GLib, GTK 3, ATK, Pango, GdkPixbuf | LGPL-2.1 | `licenses/LGPL-2.1.txt` |
| GnuTLS, libgcrypt, libidn2, p11-kit | LGPL-2.1 | `licenses/LGPL-2.1.txt` |
| ICU | Unicode licence (permissive) | — |
| FreeType | FTL or GPL-2 | `licenses/GPL-2.0.txt` |
| HarfBuzz, Graphite2 | MIT / LGPL-2.1 | `licenses/MIT.txt`, `licenses/LGPL-2.1.txt` |
| libpng, libjpeg-turbo, brotli, zlib, zstd, lz4, xz | permissive (zlib/BSD/MIT-style) | `licenses/BSD-3-Clause.txt`, `licenses/MIT.txt` |
| MIT Kerberos, OpenLDAP, libcurl, nghttp2, libpsl | permissive (MIT/BSD-style) | `licenses/MIT.txt`, `licenses/curl.txt` |
| dbus, libinput, libevdev, libudev | permissive / LGPL-2.1 | `licenses/LGPL-2.1.txt` |

**If you redistribute the bundle**, you take on the obligations of everything
inside it, not just Qt. The LGPL components require the same treatment Qt does:
dynamic linking, licence text included, and a way for the recipient to swap the
library. The bundle satisfies the mechanics of this (see below), and the licence
texts named above travel with it in `licenses/`, but you must also be able to
supply the corresponding sources.

**Distributing a `.deb`, an RPM, or a Flatpak avoids all of this**: the system
provides those libraries and the distribution has already met their terms. The
bundle exists for handing a build to someone quickly, not as the recommended
release channel.
