# ADR-0085: Mole asks once at startup whether it is out of date, and never says so twice

- **Date:** 2026-09-01
- **Status:** Accepted

## Context

[ADR-0084](0084-the-newest-release-is-stated-in-a-file-at-a-fixed-path.md) put a
machine-readable statement of the newest release at a fixed URL. Nothing read it.
This is what reads it, and almost every decision in it is about restraint rather
than about capability: the application is a file manager, the check is a background
courtesy, and every way of getting it wrong is a way of being annoying.

Three constraints came with the request, from the author, on 2026-09-01. The window
must never be held up. Somebody told about a version who does nothing must not be
told again for a week. And nothing identifying may leave the machine.

## Decision

**`Qt6::Network`, on `mole_ui`, and no HTTP client of our own.** One
`QNetworkAccessManager`, one conditional `GET`, on the event loop.

**No thread.** `QNetworkAccessManager` is asynchronous: the request goes out and a
signal comes back. The window is never blocked and there is nothing to
synchronise.

**One notice per version, then a week of silence — the request included.** What is
remembered is *which version was announced and when*, not when we last looked:

- No newer version: nothing said, the `ETag` kept, ask again next start.
- A newer version nobody has been told about: announce it, and write down the
  version and the day.
- A version already announced: nothing said, and **nothing asked either** until
  seven days after the day it was announced.

The silence stops holding the moment the announced version is the one running.
Somebody who took the notice and updated has acted on it, and keeping the silence
would hide the next release for up to a week as a reward for doing what was asked.

**The check is `UpdateCheck` in `src/ui`, owned by `AppController`, and started by
`main.cpp` once there is a window.** It is not a `Task`: the task framework is for
work a person started and can watch in `TaskStrip`.

**Failure is silence.** Unreachable, refused, timed out, a 404, a 500, a body that
stops half way, a document that will not parse, a format this build has never heard
of: one line at debug level, and nothing else. No message, nothing in the session
log at warning level, and no delay anybody notices.

**What goes on the wire is fixed, not inherited.** Measured on Qt 6.4.2, a request
with nothing set carries `Host`, `Connection: Keep-Alive`, `Accept-Encoding`,
`User-Agent: Mozilla/5.0` — and `Accept-Language`, built out of the system locale.
So two headers are set by hand: `Accept-Language: en` and `User-Agent: Mole`.
Nothing else is added — no install id, no counter, no platform, no version.

**The landing page is the manifest's own field, and it must be `https`.** The
application never assembles a URL, and it refuses to hand any other scheme to
whatever opens links on the machine.

## Reason

**Qt Network rather than libcurl, which this project already links.** libcurl is
found in `src/plugins/CMakeLists.txt` alone and linked `PRIVATE` into the optional
network plugin — the one about drives, absent whenever `CURL` is not found. Reaching
for it here would make the update check depend on an optional plugin's dependency,
and it is blocking by nature: `curl_easy_perform` occupies the thread that calls it,
which is precisely why the test suite's scripted server needs a thread of its own.
That is where the author's request for a thread came from, and with
`QNetworkAccessManager` there is nothing for a thread to do. `Qt6Network` ships in
`qtbase` — the same `qt6-base-dev` that already provides `Qt6Core` — so this adds no
package to any build, and `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` picks up the runtime
dependency on its own.

**On `mole_ui` rather than on `mole_core`.** Core is linked by the console runner
and every headless tool, and none of them has any use for an HTTP stack. It is
`PRIVATE` there and `UpdateCheck.h` forward-declares what it holds, so nothing that
links the library has to know Qt Network exists.

**The week holds the request back, not only the notice.** Holding only the notice
would be simpler and would spend a request every morning to reach a conclusion that
was already decided. The deliberate cost is that a release appearing two days into
that week is not found until the week is out — the author's trade, made knowingly.

**A version already announced is never announced again.** Not even after the week.
The week decides when to *ask* again, and what it is asking is whether there is
something new to say. A weekly repetition of the same notice is a nag, and the
conditional `GET` would not support it anyway: the answer to an unchanged file is
`304` with no body, so the reminder would have to be reconstructed from remembered
state — machinery in aid of an outcome nobody wants.

**`Accept-Language` is replaced.** It is the only thing in a default Qt request that
says anything about whoever is asking, and on a Polish machine it says `pl-PL,en,*`.
The manifest is not translated, so the header buys nothing at all. Setting it empty
does not work — Qt puts its own back — so it is set to a fixed `en`.

**The user agent is replaced too.** The default is a browser's name, which is untrue,
and the honest alternative — `Mole/0.1.0` — would be a count of installs by release
arriving at somebody else's server, which is the thing the author ruled out. A bare
`Mole` is honest about what the traffic is and says nothing about the machine.

The alternatives that lost:

- **Check on a timer, or in the background while Mole runs.** More requests for an
  answer that changes a few times a year, and a notice that can appear over
  whatever somebody is doing. Startup is the one moment an interruption is already
  expected.
- **Tell the person they are up to date.** The overwhelmingly common case is
  silence, and a popup that says "nothing to report" is a popup that trains people
  to dismiss popups. Anybody who wants that answer can look in Help.
- **Ask the GitHub API for the newest release.** ADR-0084 has the measurement: 60
  requests per IP per hour unauthenticated, and the failure looks exactly like
  being up to date.
- **Remember when we last looked.** It cannot express the rule that was asked for.
  "Told and ignored" and "looked and found nothing" want different waits, and one
  timestamp cannot hold both.
- **Download and install the new version.** A different application with a
  different threat model. Mole names a version and opens a page.

## Consequences

- **Nothing in `make test` reaches the internet.** `tst_UpdateCheck` serves the
  manifest from `tests/support/ScriptedHttpServer`, which is also the only way to
  obtain the answers that matter most — a 500, a truncated body, a connection that
  goes quiet and stays open.
- **Silence is asserted, not hoped for.** Every case in that suite ends by failing
  if anything at all was said at warning level, Qt included. That assertion caught
  a real fault immediately: the first version read the reply's body before asking
  whether the reply had arrived, and Qt answers a read on a closed device with a
  warning — a line in the session log for everybody who ever started Mole on a
  train.
- **The week of silence is checked by moving a calendar, not by waiting for one.**
  `UpdateCheck::setCalendar` exists for that and for nothing else.
- **A manifest in an unknown format is invisible to old builds.** That is the
  contract `format` exists to provide, and it means a change to that file can never
  be rolled out to copies of Mole already installed — only ignored by them.
- **The notice and the switch are not here.** `MOLE-325` connects
  `newVersionFound` to the notification the window already has and adds the Help
  entry that writes `update.check`; `MOLE-326` says all of this in `README.md`,
  which is where somebody decides whether they are comfortable with it. Until then
  the only trace of an answer is one line in the session log.
- **`Qt6Network` is on the audit list** in `docs/LICENSING.md`, which names every
  Qt module the build uses and is the evidence for the LGPL audit.
