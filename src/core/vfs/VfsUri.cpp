#include "core/vfs/VfsUri.h"

#include <QDir>
#include <QStringList>

namespace mole {
namespace {

    /// Collapses duplicate separators, resolves "." and "..", drops the
    /// trailing slash. Always returns something starting with '/'.
    QString normalisePath(QString path)
    {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));

        QStringList out;
        for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            if (part == QLatin1String("."))
                continue;
            if (part == QLatin1String("..")) {
                if (!out.isEmpty())
                    out.removeLast();
                continue;
            }
            out.append(part);
        }
        return QLatin1Char('/') + out.join(QLatin1Char('/'));
    }

} // namespace

VfsUri::VfsUri(QString scheme, QString authority, QString path)
    : m_scheme(scheme.toLower())
    , m_authority(std::move(authority))
    , m_path(normalisePath(std::move(path)))
{
}

VfsUri VfsUri::fromString(const QString& text)
{
    const int schemeEnd = text.indexOf(QLatin1String("://"));
    if (schemeEnd <= 0)
        return {};

    const QString scheme = text.left(schemeEnd);
    const QString rest = text.mid(schemeEnd + 3);

    const int pathStart = rest.indexOf(QLatin1Char('/'));
    if (pathStart < 0)
        return VfsUri(scheme, rest, QStringLiteral("/"));

    return VfsUri(scheme, rest.left(pathStart), rest.mid(pathStart));
}

VfsUri VfsUri::fromLocalPath(const QString& nativePath)
{
    return VfsUri(QStringLiteral("file"), QString(), QDir::fromNativeSeparators(nativePath));
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
    const QString base = isRoot() ? QString() : m_path;
    return VfsUri(m_scheme, m_authority, base + QLatin1Char('/') + name);
}

VfsUri VfsUri::parent() const
{
    if (!isValid() || isRoot())
        return *this;
    const int slash = m_path.lastIndexOf(QLatin1Char('/'));
    return VfsUri(m_scheme, m_authority, slash <= 0 ? QStringLiteral("/") : m_path.left(slash));
}

bool VfsUri::isWithin(const VfsUri& other) const
{
    if (m_scheme != other.m_scheme || m_authority != other.m_authority)
        return false;
    if (other.isRoot() || m_path == other.m_path)
        return true;
    return m_path.startsWith(other.m_path + QLatin1Char('/'));
}

QString VfsUri::toString() const
{
    if (!isValid())
        return {};
    return m_scheme + QLatin1String("://") + m_authority + m_path;
}

QString VfsUri::toLocalPath() const
{
    if (m_scheme != QLatin1String("file"))
        return {};
    return QDir::toNativeSeparators(m_path);
}

bool VfsUri::operator==(const VfsUri& other) const
{
    return m_scheme == other.m_scheme && m_authority == other.m_authority && m_path == other.m_path;
}

size_t qHash(const VfsUri& uri, size_t seed) noexcept
{
    return qHashMulti(seed, uri.scheme(), uri.authority(), uri.path());
}

} // namespace mole
