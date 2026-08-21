#include "core/vfs/VfsUri.h"

#include <QStringList>

namespace mole {
namespace {

    /// "C:" -- a single letter and a colon, which is every Windows drive there
    /// has ever been.
    ///
    /// Deliberately exact rather than "ends with a colon". On Linux a directory
    /// may legally be called "notes:", and the looser test would make one at the
    /// top of the filesystem into a drive root, with no name and nothing above
    /// it. A top-level directory called exactly "C:" would still be read that
    /// way, and that is the price of the spelling -- see ADR-0068.
    bool isDriveLetter(QStringView segment)
    {
        return segment.size() == 2 && segment.at(1) == QLatin1Char(':') && segment.at(0).isLetter();
    }

    /// The first path segment, empty when there is none.
    QStringView firstSegment(QStringView path)
    {
        if (path.size() < 2)
            return {};
        const QStringView rest = path.mid(1);
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        return slash < 0 ? rest : rest.left(slash);
    }

    /// How many leading segments ".." may not climb past.
    ///
    /// One for a drive and one for a share, because there is nothing above
    /// either: "C:\.." is not a place, and neither is "\\server\share\..".
    /// Everything else is zero, which is the POSIX rule of clamping at "/".
    ///
    /// Takes the segments rather than the path, because it is asked before the
    /// path has a leading slash to rely on: fromLocalPath() hands in "C:/Users".
    int floorFor(const QString& scheme, const QString& authority, const QStringList& parts)
    {
        if (scheme != QLatin1String("file"))
            return 0;
        if (!authority.isEmpty())
            return 1; // the share is the first segment and cannot be climbed out of
        for (const QString& part : parts) {
            if (part == QLatin1String("."))
                continue;
            return isDriveLetter(part) ? 1 : 0;
        }
        return 0;
    }

    /// Collapses duplicate separators, resolves "." and "..", drops the
    /// trailing slash. Always returns something starting with '/'.
    QString normalisePath(const QString& path, const QString& scheme, const QString& authority)
    {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const int floor = floorFor(scheme, authority, parts);

        QStringList out;
        for (const QString& part : parts) {
            if (part == QLatin1String("."))
                continue;
            if (part == QLatin1String("..")) {
                if (out.size() > floor)
                    out.removeLast();
                continue;
            }
            out.append(part);
        }
        return QLatin1Char('/') + out.join(QLatin1Char('/'));
    }

    /// One spelling of a string for a volume that does not distinguish case.
    ///
    /// Folded rather than compared with Qt::CaseInsensitive, and used for both
    /// the comparison and the hash, so the two agree by construction. A hash
    /// that folds differently from the equality beside it is how a QHash loses
    /// an entry it is still holding, and the disagreement would only show on
    /// characters nobody thinks to test.
    QString foldedIf(const QString& text, Qt::CaseSensitivity sensitivity)
    {
        return sensitivity == Qt::CaseSensitive ? text : text.toCaseFolded();
    }

    /// The marker that separates a uri from the version it names.
    ///
    /// Spelled out rather than a bare '?' because a '?' is a legal character in
    /// a POSIX filename -- the awkward-names suite has a `really?.txt` -- so the
    /// marker has to be something a path cannot produce. That is what the
    /// encoding below is for.
    const QLatin1String kVersionMarker("?version=");

    /// Percent-encodes the two characters that would otherwise make a uri
    /// ambiguous, and nothing else.
    ///
    /// Only two, because a uri is read by people: an encoder that also took the
    /// spaces and the accents would turn every path in every error message into
    /// something nobody can check against their own filesystem. '?' has to go
    /// because it is the marker; '%' has to go with it, or encoding would not be
    /// reversible -- a file really called `a%3Fb` would come back as `a?b`.
    QString percentEncode(const QString& text)
    {
        if (!text.contains(QLatin1Char('%')) && !text.contains(QLatin1Char('?')))
            return text;
        QString out = text;
        out.replace(QLatin1String("%"), QLatin1String("%25"));
        out.replace(QLatin1String("?"), QLatin1String("%3F"));
        return out;
    }

    /// The other direction, and deliberately only the other direction: `%41` is
    /// left as it is rather than decoded to `A`, because a file called `%41` is
    /// an ordinary file and nothing here ever wrote that sequence.
    QString percentDecode(const QString& text)
    {
        if (!text.contains(QLatin1Char('%')))
            return text;
        QString out = text;
        out.replace(QLatin1String("%3F"), QLatin1String("?"), Qt::CaseInsensitive);
        out.replace(QLatin1String("%25"), QLatin1String("%"), Qt::CaseInsensitive);
        return out;
    }

    /// '/' between segments, '\' where the platform writes one.
    QString withNativeSeparators(QString path, HostPlatform platform)
    {
        if (usesWindowsPathSyntax(platform))
            path.replace(QLatin1Char('/'), QLatin1Char('\\'));
        return path;
    }

} // namespace

VfsUri::VfsUri(QString scheme, QString authority, QString path)
    : m_scheme(scheme.toLower())
    , m_authority(std::move(authority))
    , m_path(normalisePath(path, m_scheme, m_authority))
{
}

VfsUri VfsUri::fromString(const QString& text)
{
    const int schemeEnd = text.indexOf(QLatin1String("://"));
    if (schemeEnd <= 0)
        return {};

    const QString scheme = text.left(schemeEnd);
    QString rest = text.mid(schemeEnd + 3);

    // The marker, when there is one, is the last of its kind: the token after it
    // is encoded and so carries no '?' of its own, and everything before it is a
    // path whose own '?' characters were encoded on the way out. A string
    // written before versions existed has no marker and is read exactly as it
    // always was.
    QString version;
    if (const int marker = rest.lastIndexOf(kVersionMarker); marker >= 0) {
        version = percentDecode(rest.mid(marker + kVersionMarker.size()));
        rest.truncate(marker);
    }

    const int pathStart = rest.indexOf(QLatin1Char('/'));
    if (pathStart < 0)
        return VfsUri(scheme, rest, QStringLiteral("/")).withVersion(version);

    return VfsUri(scheme, rest.left(pathStart), percentDecode(rest.mid(pathStart))).withVersion(version);
}

VfsUri VfsUri::withVersion(const QString& version) const
{
    VfsUri out = *this;
    out.m_version = version;
    return out;
}

VfsUri VfsUri::fromLocalPath(const QString& nativePath, HostPlatform platform)
{
    if (!usesWindowsPathSyntax(platform)) {
        // '\' is an ordinary character in a name here, and a leading "//" is a
        // path like any other. Nothing to read out of the spelling.
        return VfsUri(QStringLiteral("file"), QString(), nativePath);
    }

    QString path = nativePath;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // \\server\share\rest -- the server is the authority, which is what it is.
    if (path.startsWith(QLatin1String("//"))) {
        const QString rest = path.mid(2);
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash < 0)
            return VfsUri(QStringLiteral("file"), rest, QStringLiteral("/"));
        return VfsUri(QStringLiteral("file"), rest.left(slash), rest.mid(slash));
    }

    // C:\Users\ann -- the drive is the first path component, so the leading
    // slash the path always carries is simply put back on in front of it.
    return VfsUri(QStringLiteral("file"), QString(), path);
}

bool VfsUri::isRoot() const
{
    if (m_path == QLatin1String("/"))
        return true;
    if (m_scheme != QLatin1String("file"))
        return false;

    // Nothing is above a share or above a drive. Saying otherwise hands the
    // caller "/", which on Windows names nothing and no backend can list.
    const QStringView first = firstSegment(m_path);
    const bool onlySegment = first.size() + 1 == m_path.size();
    if (!m_authority.isEmpty())
        return onlySegment;
    return onlySegment && isDriveLetter(first);
}

QString VfsUri::fileName() const
{
    if (isRoot())
        return {};
    return m_path.mid(m_path.lastIndexOf(QLatin1Char('/')) + 1);
}

QString VfsUri::suffix() const
{
    const QString name = fileName();
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return {};
    return name.mid(dot + 1).toLower();
}

VfsUri VfsUri::child(const QString& name) const
{
    if (!isValid() || name.isEmpty())
        return *this;
    // Only "/" would double its slash. A drive root is "/C:" and a share root is
    // "/share": both are roots and both keep their segment, or the child would
    // land on a different volume entirely.
    const QString base = m_path == QLatin1String("/") ? QString() : m_path;
    // No version: what is inside an earlier state of a directory is a question
    // for whoever hands out the versions, and inheriting one would name a
    // version of a file that the drive never issued a version of.
    return VfsUri(m_scheme, m_authority, base + QLatin1Char('/') + name);
}

VfsUri VfsUri::parent() const
{
    if (!isValid() || isRoot())
        return *this;
    const int slash = m_path.lastIndexOf(QLatin1Char('/'));
    return VfsUri(m_scheme, m_authority, slash <= 0 ? QStringLiteral("/") : m_path.left(slash));
}

Qt::CaseSensitivity VfsUri::caseSensitivityFor(const QString& scheme, HostPlatform platform)
{
    if (scheme != QLatin1String("file"))
        return Qt::CaseSensitive;
    return platform == HostPlatform::Posix ? Qt::CaseSensitive : Qt::CaseInsensitive;
}

bool VfsUri::isWithin(const VfsUri& other) const
{
    return isWithin(other, caseSensitivityFor(m_scheme));
}

bool VfsUri::isWithin(const VfsUri& other, Qt::CaseSensitivity sensitivity) const
{
    if (m_scheme != other.m_scheme
        || foldedIf(m_authority, sensitivity) != foldedIf(other.m_authority, sensitivity))
        return false;

    const QString mine = foldedIf(m_path, sensitivity);
    const QString theirs = foldedIf(other.m_path, sensitivity);

    // "/" contains everything on this scheme and authority. A drive root and a
    // share root are roots too, but they contain only themselves and what is
    // under them -- C:\ is not above D:\, and asking isRoot() here said it was.
    if (theirs == QLatin1String("/") || mine == theirs)
        return true;
    return mine.startsWith(theirs + QLatin1Char('/'));
}

QString VfsUri::toString() const
{
    if (!isValid())
        return {};
    const QString base = m_scheme + QLatin1String("://") + m_authority + percentEncode(m_path);
    if (m_version.isEmpty())
        return base;
    return base + kVersionMarker + percentEncode(m_version);
}

QString VfsUri::toLocalPath(HostPlatform platform) const
{
    if (m_scheme != QLatin1String("file"))
        return {};

    if (!m_authority.isEmpty()) {
        // A share has a native spelling on Windows and none anywhere else.
        // Handing back the path alone would name a local directory that has
        // nothing to do with the share.
        if (!usesWindowsPathSyntax(platform))
            return {};
        return withNativeSeparators(QLatin1String("//") + m_authority + m_path, platform);
    }

    // The leading slash belongs in front of a POSIX path and in front of
    // nothing else: "/C:/Users" as a native path is "\C:\Users", which names
    // no file.
    if (!usesWindowsPathSyntax(platform) || !isDriveLetter(firstSegment(m_path)))
        return withNativeSeparators(m_path, platform);

    // "C:" and "C:\" are different places on Windows -- the first means the
    // current directory on that drive, which is whatever the process last set it
    // to. The root of a drive keeps its separator.
    const QString native = m_path.mid(1);
    return withNativeSeparators(native.size() == 2 ? native + QLatin1Char('/') : native, platform);
}

bool VfsUri::equals(const VfsUri& other) const
{
    return equals(other, caseSensitivityFor(m_scheme));
}

bool VfsUri::equals(const VfsUri& other, Qt::CaseSensitivity sensitivity) const
{
    // The version is compared as it was issued, whatever the volume does about
    // case: it is the drive's own token rather than a name on it, and two
    // spellings of one are two different versions until a drive says otherwise.
    return m_scheme == other.m_scheme && m_version == other.m_version
        && foldedIf(m_authority, sensitivity) == foldedIf(other.m_authority, sensitivity)
        && foldedIf(m_path, sensitivity) == foldedIf(other.m_path, sensitivity);
}

size_t VfsUri::hash(size_t seed) const
{
    return hash(seed, caseSensitivityFor(m_scheme));
}

size_t VfsUri::hash(size_t seed, Qt::CaseSensitivity sensitivity) const
{
    return qHashMulti(
        seed, m_scheme, foldedIf(m_authority, sensitivity), foldedIf(m_path, sensitivity), m_version);
}

QString VfsUri::canonicalKey() const
{
    const Qt::CaseSensitivity sensitivity = caseSensitivityFor(m_scheme);
    const QString base = m_scheme + QLatin1String("://") + foldedIf(m_authority, sensitivity)
        + percentEncode(foldedIf(m_path, sensitivity));
    return m_version.isEmpty() ? base : base + kVersionMarker + percentEncode(m_version);
}

bool VfsUri::operator==(const VfsUri& other) const
{
    return equals(other);
}

size_t qHash(const VfsUri& uri, size_t seed) noexcept
{
    return uri.hash(seed);
}

} // namespace mole
