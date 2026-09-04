#pragma once

#include <QString>

namespace mole {

/// A path with its trailing slashes taken off, keeping a bare root.
///
/// **Six backends stripped trailing slashes by hand**, and LocalFileSystem twice
/// more inside itself: `while (path.endsWith('/')) path.chop(1)`, with and
/// without the `size() > 1` guard that keeps `"/"` from becoming `""`. A root
/// that turns into an empty string is a listing of the wrong place, so the guard
/// is the interesting half and it was present in some copies and not others.
/// See MOLE-403.
QString withoutTrailingSlash(QString path);

/// The same, and it will take the last one too: `"/"` becomes `""`.
///
/// **The two rules are the reason this is two functions rather than one.** A
/// *root* that loses its last slash is a listing of nowhere, which is what the
/// `size() > 1` guard above is for. A *prefix* -- an S3 key prefix, an SMB share
/// name, an endpoint -- has no such floor: empty means "the whole bucket", and
/// keeping a lone separator there would make every key start with two. Both were
/// written by hand in the backends, and which of the two a copy implemented
/// depended on whether the guard had been pasted with it.
QString withoutAnyTrailingSlash(QString path);

/// Whether a name is hidden by the leading-dot convention.
///
/// Every remote backend decides this way, because the protocols carry no such
/// flag: SFTP, FTP, S3, WebDAV, SMB, NFS and the in-memory drive all ask whether
/// the name begins with a dot. **LocalFileSystem does not** -- it asks
/// `QFileInfo::isHidden()`, which on Windows reads the file's attribute and on
/// Unix is the same dot rule. That difference is deliberate and is the reason
/// this is not used there: a local file marked hidden by the filesystem is
/// hidden whatever it is called.
bool looksHidden(const QString& name);

/// The local path a mountable backend puts in a uri's authority, and the way
/// back.
///
/// An archive addresses itself in the authority of its members' uris --
/// `archive://%2Fhome%2Fsomebody%2Freports.zip/notes.txt` -- and so does any
/// other backend built from a file through `IFileSystemFactory::configForFile()`.
/// The encoding is one decision and it was made in three places: the archive
/// plugin owns it, and `FileListModel` and `SearchFeatures` each decoded an
/// authority by hand with `QUrl::fromPercentEncoding`. Those two are in layers
/// that cannot see the plugin, which is how the copies came to exist -- so the
/// pair lives here, where the uri does. See MOLE-403.
QString authorityFromLocalPath(const QString& path);
QString localPathFromAuthority(const QString& authority);

} // namespace mole
