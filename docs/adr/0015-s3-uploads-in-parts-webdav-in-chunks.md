# ADR-0015: S3 uploads in parts, WebDAV in chunks

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

[ADR-0014](0014-remote-files-stream-rather-than-stage.md) made SFTP stream in
both directions and left the other two backends staging their uploads, on the
grounds that the protocols require it: S3 signs a payload hash and a length it
must know before sending, and WebDAV servers are not uniformly willing to accept
a chunked `PUT`. Both reasons are real, and neither survives contact with a file
that is larger than the local scratch space. A backup that cannot be sent
anywhere is not helped by the protocol having a good excuse.

S3 also had a second limit from the same cause: one `PUT` is one object, capped
at 5 GB on AWS, so anything larger could not be written at all.

## Decision

**S3 sends an object it cannot measure as a multipart upload** -- begin, parts of
64 MiB, complete -- staging one part at a time so each can be measured and
signed. A write whose size is known and fits in a part is still a single signed
`PUT`. A write whose size is not known starts down the multipart path and drops
back to a single `PUT` when the payload turns out to end inside the first part,
so nothing pays for three requests to store a small file.

**A failed multipart upload is abandoned**, which is not tidiness: the parts sit
in the bucket and are charged for until something removes them.

**Completion is checked against the body, not the status.** S3 answers
`CompleteMultipartUpload` with 200 and puts the failure inside the document.

**WebDAV streams a large write with a chunked transfer encoding**, and only a
large one. A write that is small or of unknown size keeps the staged `PUT` with
an exact `Content-Length` that has always worked.

## Reason

**Why multipart rather than `UNSIGNED-PAYLOAD`.** Not signing the payload would
avoid hashing it, but S3 still requires `Content-Length` on a `PUT`, so the
length would still have to be known before sending -- which is the actual
obstacle. Multipart removes it, and lifts the 5 GB ceiling as a side effect.

**Why 64 MiB parts.** S3 puts a floor of 5 MiB under every part but the last and
a ceiling of ten thousand parts on an object. 64 MiB puts the object ceiling at
640 GB, past anything a file manager will be handed, while the staging cost of
one part stays beneath notice.

**Why WebDAV only streams above a threshold.** A chunked `PUT` is the only way to
send something too large to stage, and it is also the request some servers
refuse with 411. Making it the exception rather than the rule means the
possibility of that refusal is confined to the case that has no alternative,
instead of being introduced to every small write that works today.

## Consequences

- An object of any size can be written to S3. Verified against Backblaze B2: a
  150 MB object goes up in parts and comes back byte for byte, and the
  conformance suite still passes for everything smaller.
- **The WebDAV path is written but unverified.** There is still no WebDAV server
  to test against -- the same gap TODO.md has recorded since the backend was
  written. A server that answers 411 will fail the write with the server's own
  words, which is at least a diagnosable failure rather than a silent one.
- FTP still stages. It is not in anybody's way, and changing a backend nobody is
  blocked on is change for its own sake.
