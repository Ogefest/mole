# Mole

**An IDE, but for files.**

An editor gives a programmer one window in which to search a project, open
several things at once, run tools over them and see the results — without ever
reaching for a terminal or a second application. Mole sets out to be that window
for files: browsing, searching, comparing, syncing, renaming in bulk, finding
duplicates, previewing anything, and running long jobs in the background, all
under one keyboard.

It is native and desktop-first, built on C++20 and Qt 6 / QML, and designed from
the start so that **drives** and **tabs** are both plugin extension points: a tab
is one workflow over files, and anything that can list and read entries can be a
drive.

Status: early. The architecture is in place and covered by tests; features are
being added on top of it.

## What works today

- **Virtual drives.** Local disk, an in-memory scratch space and read-only
  archives (zip, tar, 7z, ...) are all mounts of the same `IFileSystem`
  interface. Activating an archive mounts it and browses inside.
- **Tabs as workflows.** Browser (single pane, dual pane, a grid of icons or a
  gallery of thumbnails), search, previews, reports and the rest — each a
  separate feature, opened from the same menu.
- **Filter by typing.** Start typing in a listing and it narrows to what you
  typed — no shortcut, no storage access. A search that walks the tree is a
  separate tab on `Ctrl+F`.
- **One search, asked a dozen ways.** A name as text, a shape or an expression;
  a path; a size; dates typed the way people say them (`last 7 days`, `>30d`);
  what a file *is* rather than what it is called; folders to skip; and the text
  inside the files themselves. Where to look is a field — this folder,
  everywhere indexed, or a path — and a query line above the form is the same
  query seen twice, so `report ext:pdf size>10M` is the whole of it.
- **Preview with F3.** Source code coloured for twenty-odd languages, Markdown
  rendered, images, video paused at its first frame, CSV/TSV, SQLite databases and
  Parquet files as a grid with filtering and copyable cells, and the bytes of
  anything else. What a file *is*
  comes from what is in it rather than from its name, so a `Dockerfile` opens as
  a coloured Dockerfile. Every viewer has a details panel: a photograph's camera
  and exposure, a document's author, a video's codecs, an audio file's tags. A
  file of any size opens at once — only the part being shown is ever read. Left
  and right step through the folder.
- **Commander-style file operations.** Copy and move between panes with F5/F6,
  across *any* two drives; rename, create and delete with F2/F7/F8.
- **Dragging in and out.** Files leave a listing by pointer into anything that
  takes files, and arrive the same way into the folder a pane is showing —
  always as a copy, asking before it overwrites a name. A row that is not on
  this computer is fetched first, so an archive and a bucket can be dragged out
  of too.
- **Git state, read and never written.** A folder inside a checkout says which
  branch, how far it is from what it tracks, what the last commit was and how much
  has changed; the rows carry git's own letters, and a folder says when something
  inside it has changed. Refreshed when an operation writes or when you commit from
  the terminal panel. Local drives, and it needs libgit2 — without it Mole behaves
  exactly as it did before. Mole shows git state and does not change it.
- **Full keyboard control.** Arrows, Enter, Insert to select, function keys for
  operations. Enter hands a file to the desktop's default application.
- **Background everything.** Listing, scanning and searching run on a thread
  pool with progress and cancellation. The UI thread never touches storage.
- **A file index.** Scan a tree once, then search it instantly without going
  back to the disk (or the network). A scan can also record what each file says
  about itself — a camera, an author, a duration — and list what is inside the
  archives it meets; the contents themselves are deliberately never indexed. A
  folder with an indexed subfolder is answered by both at once, and every row
  says whether it is what is on disk now or what a scan remembered. Re-scans keep
  what has not changed, and a folder can be put on a clock at any interval. The
  *Indexes* tab lists every index there is — how old each one is, what kind of
  scan built it, whether anything is keeping it fresh — and is where one is
  rescanned, stopped or thrown away.
- **Thumbnails, and a gallery to put them in.** Pictures, PDF first pages and
  video frames, made off the UI thread, a few at a time, only for what is on
  screen, and cached in memory and on disk. On a drive that is not local the
  thumbnail a camera wrote into the file is read instead of the file — kilobytes
  rather than megabytes — and anything past a ceiling is left alone.
- **Sessions.** Open tabs, their folders, their layout and the window's own
  size come back after a restart. Each tab kind decides what it remembers,
  plugins included.
- **Drives and bookmarks.** The sidebar lists what the machine actually has
  mounted — with how full each one is, amber past three quarters and red past
  nine tenths — plus the places you saved yourself. Drives with no meaningful
  capacity simply show a name.
- **Directory reports.** Analyse one folder or several, each with its own
  history, and diff any run against an earlier one.
- **Jobs on a clock.** Put a report on a repeat and it runs in the background,
  including catching up on a run missed while the machine was off. A tracking
  tab (`Ctrl+Shift+J`) shows every attempt and puts failures first.
- **Sync between any two drives.** Update, mirror or fill gaps, with filters and
  a dry run that is the real plan with the last step withheld.
- **Duplicate detection.** Four strategies, staged so the expensive comparison
  only ever sees what the cheap ones could not rule out.
- **Bulk rename.** Composable operations with a live preview, and a batch that
  would collide is refused rather than half-applied.
- **File sets.** A hand-built list of files across any number of drives that
  every operation accepts exactly as it accepts a selection.
- **A terminal panel.** A shell for the folder you are looking at, along the
  bottom of the window.
- **Network drives.** SFTP, FTP and FTPS, S3, WebDAV, SMB and NFS, each configured
  from a short form the backend itself describes. One S3 engine serves AWS, Backblaze
  B2, MinIO, Ceph, Wasabi and R2 — the endpoint and the addressing style are just
  fields. A Windows or NAS share and an NFS export are reached in-application, with
  no mount and no root. They ship as a loadable plugin, and further backends arrive
  the same way.
- **Credentials encrypted and portable.** Passwords live in an AES-256-GCM store
  keyed by a passphrase you choose, never in the settings file. It is not tied to
  this machine: back up the configuration and the same passphrase opens it on a
  fresh install.
- **A plugin API** with a loadable-shared-library path, used by the shipped
  archive and network plugins.

## Building

Requires a C++20 compiler, CMake 3.24+ and Qt 6.4+.

On Ubuntu 24.04:

```sh
sudo apt install -y build-essential ninja-build ccache pkg-config git \
  qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
  qt6-tools-dev qt6-tools-dev-tools qt6-svg-dev libqt6sql6-sqlite \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-templates qml6-module-qtquick-window \
  qml6-module-qtqml-workerscript qml6-module-qtquick-shapes \
  libarchive-dev libcurl4-openssl-dev libssl-dev

make build      # binary lands in build/debug/mole
make run
make test
```

Optional but recommended while developing:

```sh
sudo apt install -y clang-format clang-tidy cppcheck valgrind
```

`ccache` is optional too, and nothing asks for it: the build looks for it and uses
it when it is there. It used to be named in the base preset, which made a machine
without it fail to configure at all.

`libarchive-dev` is optional: without it the archive plugin is skipped and the
rest of the application builds normally. The same goes for `libcurl4-openssl-dev`
and `libssl-dev`, without which the network plugin is skipped and there are no
SFTP, FTP, S3 or WebDAV drives — configure says so, rather than offering drives
that cannot connect. `libsmbclient-dev` and `libnfs-dev` are optional in the same
way and one step finer: without either, the plugin still ships and only that drive
kind is missing, which configure names.

### Getting a binary

`make build` already produces one at `build/debug/mole`, but it is a
debug build that only runs from the build tree. For anything else:

```sh
make release     # optimised, still run from build/release/
make install     # into /usr/local — override with PREFIX=~/.local
make bundle      # self-contained folder in dist/, runs without Qt installed
```

| | Size | Needs Qt on the target | Use it for |
|---|---|---|---|
| `make release` | 24 MB | yes | running on this machine |
| `make install` | 24 MB | yes | your own machine, or building a `.deb` |
| `make bundle` | 68 MB | **no** | handing it to someone else |

`make install` lays out `<prefix>/bin/mole` and
`<prefix>/lib/mole/plugins`, plus a desktop entry and icon so the
app shows up in the launcher. The binary finds its plugins relative to itself,
so any prefix works without rebuilding.

`make bundle` additionally copies Qt, the platform plugin, the SQLite driver
and the QML modules into `dist/`, behind a launcher script that points Qt at
them. Ship the whole `dist/` folder; the entry point is `dist/mole`.

Check a packaged build without a display:

```sh
./dist/mole --plugins    # prints the search path and what loaded
```

### Make targets

| Target | Does |
|---|---|
| `make build` | configure (if needed) and compile |
| `make run` | build and launch |
| `make install` | install into `PREFIX` (default `/usr/local`) |
| `make uninstall` | remove an installed copy |
| `make bundle` | self-contained folder in `dist/` |
| `make test` | build and run the whole suite in parallel |
| `make asan` | build and test under AddressSanitizer + UBSan + leak detection |
| `make screenshots` | drive the real interface headlessly and photograph each verified state |
| `make guide-images` | the same, copied into the user guide |
| `make release` | optimised build with debug info |
| `make format` | apply `.clang-format` across the tree |
| `make tidy` | run `clang-tidy` over the compilation database |
| `make help` | list all targets |

#### Without `make`

The `Makefile` is a thin wrapper over CMake presets, and it is GNU make with shell
recipes — which is not the way in on Windows. Nothing needs porting, because the
presets are the real build configuration and CMake drives them directly:

```sh
cmake --preset debug          # make build, first half
cmake --build build/debug     # make build, second half
ctest --test-dir build/debug --output-on-failure   # make test
```

`base` fixes the generator to Ninja, which exists on all three systems. On Windows
that means running these from a developer command prompt, where the MSVC
environment has already been set — Ninja finds no compiler otherwise.

## Keyboard

| | |
|---|---|
| `Ctrl+T` / `Ctrl+Shift+T` | new browser tab / new dual-pane tab |
| `Ctrl+W`, `Ctrl+Tab`, `Ctrl+1…9` | close, cycle, jump to tab |
| `F4` | open the application menu (arrows and Enter from there) |
| *just start typing* | filter this folder |
| `F3` | preview the file under the cursor |
| `Ctrl+F` / `Ctrl+Shift+I` | search this folder / search everywhere indexed (new tab) |
| `Ctrl+Shift+C` / `Ctrl+Shift+F` | copy this folder's path / the selected file's path |
| `Ctrl+D` | bookmark this folder — or, in the Sets tab, the set you are looking at |
| `Ctrl+G` or `Ctrl+L` | type a destination in the path bar |
| `Ctrl+←` / `Ctrl+→` / `Ctrl+↑` | back / forward / up |
| `↑ ↓ PgUp PgDn Home End` | move the cursor |
| `Enter` | open — folder, archive as a drive, or the default application |
| `Backspace` | go up one folder |
| `Tab` | switch pane (dual mode) |
| `Insert` / `Space` / `Ctrl+A` / `*` | tick and advance / tick / select all / invert |
| `F2` `F5` `F6` `F7` `F8` | rename, copy, move, new folder, delete |

The `⌨` button in the toolbar shows the same list in the app.

In the **Sets** tab the same keys do the same kind of thing to a set's members: the
arrows walk them, `Enter` opens the member itself in a browser with the cursor already
on it, and `F3` previews it without leaving the set. A member whose file has gone says
so rather than opening the folder it used to be in.

The first block works wherever the keyboard is; so do `F3` and `Ctrl+←/→/↑`,
because what they act on is the pane in front of you rather than whatever holds
the keyboard. The rest need the listing focused — cursor keys, typing to filter
and ticking rows are about the thing you are looking at, and `F2` `F5` `F6` `F7`
`F8` need to know which pane they are working *from*. See
[ADR-0019](docs/adr/0019-the-keys-that-belong-to-the-window.md).

## Layout

```
src/core       VFS, tasks, event bus, index      (headless, QtCore + QtSql)
src/sdk        the published plugin API
src/host       plugin loader and registries
src/ui         list models and controllers       (headless)
src/plugins    built-in features + archive plugin
src/app        main() and the QML shell
src/tools      mole-tasks, the console runner       (no GUI)
tests          one binary per unit, plus an integration suite
tests/scripts  the shell scripts, run against a stub ssh
```

## Using it

- [`docs/guide/`](docs/guide/README.md) — the user guide: browsing, previewing,
  searching, and the operations. Every picture in it was taken by the test suite
  immediately after asserting that what it shows is real, so it cannot document a
  feature that has stopped working.

### From a console

`mole-tasks` runs any of the work without a window — for reproducing something
against a real server, for driving a transfer under `tc netem`, and for a machine
with no display. It reads the same drives, credentials and index the application
does. See [ADR-0028](docs/adr/0028-a-console-runner-for-the-tasks.md).

```sh
mole-tasks copy --from file:///data/big.bin --to file:///backup --log net
mole-tasks sync --from file:///photos --to nas:///photos --mode mirror   # says what it would do
mole-tasks sync --from file:///photos --to nas:///photos --mode mirror --apply
mole-tasks duplicates file:///downloads --by content
mole-tasks drives                                                        # what is mounted
mole-tasks help                                                          # every command
```

A drive that needs configuring comes from the store the application writes
(`--drive «name»`, with the passphrase in `MOLE_PASSPHRASE`) or from the command
line (`--mount name=nas,type=sftp,host=…,user=…,password=@SFTP_PASSWORD`). A
value written `@SOMETHING` is read from that environment variable, so a password
stays out of the argument list and out of the shell history.

Exit codes: `0` done, `1` something in the work failed, `2` the command line is
wrong, `3` a drive could not be reached, `130` interrupted.

## Extending it

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — how the pieces fit and why.
- [`docs/WRITING_PLUGINS.md`](docs/WRITING_PLUGINS.md) — adding a drive, a tab
  or a preview.

The extension points:

| To add | Implement |
|---|---|
| a drive (SFTP, S3, WebDAV, SMB, NFS, a git forge) | `IFileSystemFactory` + `IFileSystem` |
| a tab (duplicates, analytics, bulk rename) | `IFeature` + `FeatureController` |
| a viewer (PDF, SQLite, Parquet, bytes) | `IPreviewProvider` + `PreviewController` |
| what a file says about itself (EXIF, tags) | `IMetadataReader` |
| a menu entry under File / View / Tools / Help | `MenuAction` |

Built-in functionality is registered through the identical `PluginRegistry`
that third-party code uses, so the API cannot quietly rot.

## Environment variables

| Variable | Effect |
|---|---|
| `MOLE_PLUGIN_PATH` | extra directories to search for plugins (`:`-separated) |
| `MOLE_INDEX_PATH` | where the SQLite index lives, instead of the user profile |
| `MOLE_SESSION_PATH` | where the open-tabs file lives, instead of the user profile |
| `MOLE_BOOKMARKS_PATH` | where the bookmarks file lives, instead of the user profile |
| `MOLE_ANALYSIS_PATH` | where saved reports live, instead of the user profile |
| `MOLE_SCHEDULE_PATH` | where the job schedule lives, instead of the user profile |
| `MOLE_ALERTS_PATH` | where watched metrics live, instead of the user profile |
| `MOLE_SETS_PATH` | where file sets live, instead of the user profile |
| `MOLE_SECRETS_PATH` | where encrypted credentials live, instead of the user profile |
| `MOLE_REMOTES_PATH` | where configured drives live, instead of the user profile |
| `MOLE_PLUGIN_PATH` | extra directories to load plugins from |
| `MOLE_LOG_PATH` | where the session log is written, instead of the user profile |
| `MOLE_LOG` | what to record in detail: `task`, `drive`, `net`, `curl`, or `all` |
| `MOLE_PASSPHRASE` | unlocks the credential store for `mole-tasks`, which has nobody to ask |
| `MOLE_SCREENSHOT_DIR` | where `tst_Walkthrough` writes its pictures |

### When something goes wrong

Every run writes a session log next to the rest of the profile, keeping the previous
run alongside it, and a crash puts its backtrace in the same file. It records what
the application said, which for an operation that misbehaved is usually not enough —
so `MOLE_LOG` turns up the detail by subject, into the same file:

```sh
MOLE_LOG=net,curl mole      # every network transfer, and libcurl's own account of it
MOLE_LOG=task,drive mole    # what every job did, and every operation on every drive
MOLE_LOG=all mole           # all four
```

`task` and `drive` are written in one place each and so cover every job and every
drive, including ones a plugin brings. Credentials in header lines are redacted: a
log is a thing people send to each other.

### Testing the network backends against a real server

The network backends carry conformance tests that talk to an actual server, and
they skip themselves when there is nothing to talk to — so `make test` is green on
a machine with no account, and no credential is ever committed. To run them, put
an account in the environment:

```sh
export MOLE_TEST_SFTP_HOST=… MOLE_TEST_SFTP_USER=… MOLE_TEST_SFTP_PASS=…
export MOLE_TEST_FTP_HOST=…  MOLE_TEST_FTP_USER=…  MOLE_TEST_FTP_PASS=…
export MOLE_TEST_S3_KEY_ID=… MOLE_TEST_S3_SECRET=… MOLE_TEST_S3_BUCKET=…
export MOLE_TEST_S3_REGION=… MOLE_TEST_S3_ENDPOINT=…
export MOLE_TEST_WEBDAV_URL=… MOLE_TEST_WEBDAV_USER=… MOLE_TEST_WEBDAV_PASS=…
make test
```

Each suite works under a uniquely named directory or key prefix and removes it
afterwards, so it is safe to point at a bucket or share that holds real files.
`MOLE_TEST_SFTP_BASE`, `MOLE_TEST_FTP_BASE` and `MOLE_TEST_FTP_PORT` override where
it works and how it connects. Use a throwaway account: these are test credentials
in a shell history.

### The scale tier

`make test-heavy` is the same thing at a size that hurts: gigabytes each way,
between local disk and every configured backend, with the payload verified byte
for byte against what belongs at each offset. It asserts more than "the copy
worked" —

- **peak temporary space**, which is the check that would have caught staging
  before it became a wall (see [ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md));
  a copy that streams needs room for a chunk, one that stages needs room for the
  whole file, and nothing else tells them apart
- **resident memory and file descriptors**, so a leak shows up here rather than
  after eight hours of somebody's backup
- **throughput per run**, recorded to a file, so a regression in speed is as
  visible as one in correctness

It asks the test machine how much room each destination has rather than being
told, and a destination that cannot hold the payload is a **skip with the
reason** — filling a test machine's disk would take every other suite down with
it. `MOLE_TEST_HEAVY_BYTES` sets the size; the default is 10 GiB.

The SFTP and S3 suites also carry large-file tests, because that is where transfers
break and a few kilobytes prove nothing. SFTP puts 64 MB on the server and reads it
back; `MOLE_TEST_SFTP_LARGE_MB` changes the size, and `MOLE_TEST_SFTP_LARGE_PATH`
points it at a file that is already there, which writes nothing at all. S3 writes an
object past the part size so the upload is a multipart one, sized by
`MOLE_TEST_S3_LARGE_MB`.

## Roadmap

Backends: SFTP, FTP, S3, WebDAV, SMB and NFS all ship today. Google Drive, Dropbox
and OneDrive speak proprietary APIs and would each need a plugin of their own — see
[ADR-0011](docs/adr/0011-network-drives-without-rclone.md).
Previews: DuckDB tables. PDF, SQLite, Parquet and video playback are done; so are
image metadata and audio tags, which read a header rather than linking `exiv2` or
`taglib` — a reader is handed a bounded prefix of a file that may be on a remote
drive, and neither library works that way. See
[ADR-0034](docs/adr/0034-what-a-file-says-about-itself.md).

Cross-platform: Mole is written to be portable, but it has only ever been
configured, built and run on Linux. The platform-specific pieces found so far are
being worked through; see [TODO.md](TODO.md) for where that stands.

## Licence

**Apache-2.0** — see `LICENSE`.

Qt is used under the **LGPL-3.0**, dynamically linked and unmodified. Plugins
may be released under any licence their author prefers.

`make licence-check` verifies the conditions that make this combination valid;
[`docs/LICENSING.md`](docs/LICENSING.md) explains each one and what you still
have to do when publishing a build. Third-party components are listed in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
