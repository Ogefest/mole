#pragma once

#include <QHashFunctions>
#include <QMetaType>
#include <QString>

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
class VfsUri
{
public:
    VfsUri() = default;
    VfsUri(QString scheme, QString authority, QString path);

    /// Parses "scheme://authority/path". Returns an invalid uri on garbage.
    static VfsUri fromString(const QString& text);

    /// Wraps a native path (accepts both '\' and '/') as a file:// uri.
    static VfsUri fromLocalPath(const QString& nativePath);

    bool isValid() const { return !m_scheme.isEmpty(); }
    bool isRoot() const { return m_path == QLatin1String("/"); }

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

    QString toString() const;
    /// Native path for file:// uris, empty string for anything else.
    QString toLocalPath() const;

    bool operator==(const VfsUri& other) const;
    bool operator!=(const VfsUri& other) const { return !(*this == other); }

private:
    QString m_scheme;
    QString m_authority;
    QString m_path;
};

size_t qHash(const VfsUri& uri, size_t seed = 0) noexcept;

} // namespace mole

Q_DECLARE_METATYPE(mole::VfsUri)
