# Third-party notices

Mole itself is licensed under Apache-2.0 (see `LICENSE`). It builds
on the components below, none of which are part of this project.

## Linked at build time

| Component | Licence | How it is used |
|---|---|---|
| **Qt 6** (Core, Gui, Qml, Quick, QuickControls2, Sql) | LGPL-3.0 | Dynamically linked. Full text: `licenses/LGPL-3.0.txt` |
| **Qt 6 Pdf** | LGPL-3.0 | Dynamically linked; renders PDF previews. Embeds **PDFium** (BSD-3-Clause, Copyright The Chromium Authors) and PDFium's own third-party components — FreeType, libpng, libtiff, lcms, OpenJPEG and others, under BSD, Apache-2.0 and similar permissive terms. All of it lives inside `libQt6Pdf.so`, not in Mole's binary. |
| **Qt Test** | LGPL-3.0 | Test binaries only; never shipped in a release |
| **libarchive** | BSD-2-Clause | Dynamically linked by the archive plugin |
| **libcurl** | curl licence (MIT/X-style) | Dynamically linked by the network plugin; carries SFTP, FTP/FTPS, S3 and WebDAV. Uses whatever SSH and TLS libraries the system's curl was built against |
| **xxHash** | BSD-2-Clause | Dynamically linked when present; digests the head of a candidate file in a duplicate scan, which is a filter and not a proof. Optional — without it the filter falls back to SHA-256 |
| **OpenSSL** | Apache-2.0 | Dynamically linked; AES-256-GCM for the credential store and HMAC-SHA256 for S3 request signing |
| **SQLite** | Public domain | Reached through Qt's `QSQLITE` driver, not linked directly |

Qt is used **unmodified**. No Qt source file is copied into this repository.

## Additionally present in `make bundle` output

The self-contained bundle copies whatever the build machine's Qt depends on.
On a typical Linux system that is roughly 120 shared libraries, including:

| Component | Licence |
|---|---|
| glibc, libstdc++ (**not** bundled — taken from the host) | LGPL-2.1 / GPL-3 with exception |
| GLib, GTK 3, ATK, Pango, GdkPixbuf | LGPL-2.1 |
| GnuTLS, libgcrypt, libidn2, p11-kit | LGPL-2.1 |
| ICU | Unicode licence (permissive) |
| FreeType | FTL or GPL-2 |
| HarfBuzz, Graphite2 | MIT / LGPL-2.1 |
| libpng, libjpeg-turbo, brotli, zlib, zstd, lz4, xz | permissive (zlib/BSD/MIT-style) |
| MIT Kerberos, OpenLDAP, libcurl, nghttp2, libpsl | permissive (MIT/BSD-style) |
| dbus, libinput, libevdev, libudev | permissive / LGPL-2.1 |

**If you redistribute the bundle**, you take on the obligations of everything
inside it, not just Qt. The LGPL components require the same treatment Qt does:
dynamic linking, licence text included, and a way for the recipient to swap the
library. The bundle satisfies the mechanics of this (see below), but you must
also ship the licence texts and be able to supply the corresponding sources.

**Distributing a `.deb`, an RPM, or a Flatpak avoids all of this**: the system
provides those libraries and the distribution has already met their terms. The
bundle exists for handing a build to someone quickly, not as the recommended
release channel.
