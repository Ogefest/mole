#pragma once

namespace mole {

/// What a scan was asked for, in one place.
///
/// Three booleans and nothing more, because this is the *request* rather than
/// the machinery: how a scan reads a file's metadata, or lists what is inside a
/// container, is a plugin's business and cannot be named here -- `core` links
/// QtCore and QtSql only. See docs/adr/0056-a-scan-is-asked-for-in-one-place.md.
///
/// It exists because a scan used to be asked for three times over, once per
/// caller, and the callers disagreed: the nightly re-index quietly dropped the
/// metadata and the archive rows the scan that created it had recorded.
struct ScanOptions
{
    /// Keeps what has not changed rather than rewriting it. See
    /// `ScanTask::setOptions()` for what makes that correct as well as fast.
    bool incremental = false;
    /// Records what each file says about itself: EXIF, document authors, media
    /// tags. Off by default because the cost is bounded per file and unbounded
    /// in aggregate. See ADR-0039.
    bool metadata = false;
    /// Records what lives inside a file that is a container, so a zip of
    /// years-old projects can be searched without being opened.
    bool archives = false;
};

} // namespace mole
