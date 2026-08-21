#pragma once

#include "core/platform/HostPlatform.h"

#include <QHashFunctions>
#include <QMetaType>
#include <QString>
#include <Qt>

namespace mole {

/// Addresses a node in any mounted filesystem: <scheme>://<authority>/<path>.
///
///   file:///home/user/notes.txt
///   sftp://user@nas.local/volume1/photos
///   s3://my-bucket/reports/2026
///
/// The path is always normalised: it starts with '/', uses '/' as separator
/// (also on Windows) and carries no trailing slash except for the root.
/// Authority is opaque to the core -- only the owning backend interprets it.
///
/// A local path with a drive letter or a UNC share is spelled the way a file:
/// uri is spelled everywhere else, and ADR-0068 says why:
///
///   C:\Users\ann        ->  file:///C:/Users/ann
///   \\server\share\a    ->  file://server/share/a
///
/// The drive letter is the first path component, so the leading slash stays and
/// nothing else in the class has to learn about drives. What does change is where
/// walking up stops: there is nothing above C:\ or above a share, so both are
/// roots, and parent() of one is itself rather than "/" -- which is a location no
/// backend could ever list.
class VfsUri
{
public:
    VfsUri() = default;
    VfsUri(QString scheme, QString authority, QString path);

    /// Parses "scheme://authority/path", and the "?version=<token>" a versioned
    /// one carries. Returns an invalid uri on garbage.
    static VfsUri fromString(const QString& text);

    /// Wraps a native path (accepts both '\' and '/') as a file:// uri.
    ///
    /// `platform` decides how the path is read -- whether a leading "C:" is a
    /// drive and a leading "\\" is a share, or whether both are just characters
    /// in a name, which on Linux they are. It defaults to the system this build
    /// targets, so callers pass nothing; a test passes Windows and gets the
    /// Windows answer on any machine.
    static VfsUri fromLocalPath(const QString& nativePath, HostPlatform platform = hostPlatform());

    bool isValid() const { return !m_scheme.isEmpty(); }

    /// Which state of the file is meant, when it is not the current one.
    ///
    /// Opaque: a snapshot name on one drive, an object's version id on another,
    /// and nothing above the backend that issued it ever reads it. Empty means
    /// the file as it is now, which is what every uri in Mole meant until this
    /// existed.
    ///
    /// It is part of the uri rather than something carried beside it because
    /// that is what makes an earlier version **an ordinary readable uri**: F3,
    /// F5, a diff and every viewer already work on one, and a bookmark or a
    /// restored session is a string. See ADR-0077.
    const QString& version() const { return m_version; }
    bool hasVersion() const { return !m_version.isEmpty(); }

    /// The same node, at `version`. An empty token gives the current file back.
    VfsUri withVersion(const QString& version) const;
    /// The same node as it is now.
    VfsUri withoutVersion() const { return withVersion(QString()); }
    /// Whether there is anything above this. "/" for every scheme, and for a
    /// local uri also a drive root ("/C:") or a UNC share ("//server/share"),
    /// because on Windows there is nothing above either.
    bool isRoot() const;

    const QString& scheme() const { return m_scheme; }
    const QString& authority() const { return m_authority; }
    const QString& path() const { return m_path; }

    /// Last path segment, empty for the root.
    QString fileName() const;
    /// Lowercased extension without the dot, empty when there is none.
    QString suffix() const;

    VfsUri child(const QString& name) const;
    VfsUri parent() const;

    /// True when this uri is `other` or lives underneath it.
    bool isWithin(const VfsUri& other) const;
    bool isWithin(const VfsUri& other, Qt::CaseSensitivity sensitivity) const;

    /// Whether two spellings name the same node.
    ///
    /// Case folding is a property of the volume, not of this class: S3 is
    /// case-sensitive, SFTP usually is, an NTFS volume is not, and an APFS one
    /// can be either. So it is an argument, and the one-argument form asks
    /// caseSensitivityFor() for the answer that fits this uri's scheme on this
    /// platform. A caller that knows better -- one holding the backend, which is
    /// the only thing that really knows -- passes it in.
    bool equals(const VfsUri& other) const;
    bool equals(const VfsUri& other, Qt::CaseSensitivity sensitivity) const;

    /// Hashes to match equals() at the same sensitivity. Folding one and not the
    /// other is what makes a QHash lose an entry it is holding.
    size_t hash(size_t seed) const;
    size_t hash(size_t seed, Qt::CaseSensitivity sensitivity) const;

    /// One spelling per node, for anything that keys by the text of a uri rather
    /// than by the value. Two uris that are equal() have the same key.
    QString canonicalKey() const;

    /// What a scheme's paths do about case on this platform, in the absence of a
    /// backend to ask. A local path follows the platform -- NTFS and a default
    /// APFS volume fold, ext4 does not -- and every remote scheme is
    /// case-sensitive, because the protocols are, wherever the client runs.
    static Qt::CaseSensitivity caseSensitivityFor(
        const QString& scheme, HostPlatform platform = hostPlatform());

    /// The uri as text, which is what everything that stores one keeps: a
    /// bookmark, a restored session, a file set, a task's title.
    ///
    /// A version is written as `?version=<token>`, and `%` and `?` inside the
    /// path are percent-encoded so that the marker cannot be confused with a
    /// file whose name contains one. Reading it back with fromString() gives
    /// exactly what was written.
    QString toString() const;
    /// Native path for file:// uris, empty string for anything else.
    ///
    /// `platform` decides the spelling, and it has to: a drive letter loses its
    /// leading slash, a share becomes "\\server\share", and the separator is a
    /// backslash -- none of which is true anywhere else. Empty for a UNC uri
    /// asked for a POSIX path, because a share has no native path there.
    QString toLocalPath(HostPlatform platform = hostPlatform()) const;

    bool operator==(const VfsUri& other) const;
    bool operator!=(const VfsUri& other) const { return !(*this == other); }

private:
    QString m_scheme;
    QString m_authority;
    QString m_path;
    QString m_version;
};

size_t qHash(const VfsUri& uri, size_t seed = 0) noexcept;

} // namespace mole

Q_DECLARE_METATYPE(mole::VfsUri)
