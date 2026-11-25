#include "core/vfs/DiskIdentity.h"

#include <QDir>
#include <QFileInfo>

namespace mole {

namespace {

    /// The entry in `directory` whose link leads to `target`, by its name.
    QString linkTo(const QString& directory, const QString& target)
    {
        const QDir dir(directory);
        if (!dir.exists())
            return {};
        const QFileInfoList links = dir.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo& link : links) {
            if (link.isSymLink() && QFileInfo(link.symLinkTarget()).canonicalFilePath() == target)
                return link.fileName();
        }
        return {};
    }

} // namespace

DiskIdentity diskIdentityOf(const QString& device, const QString& diskDirectory)
{
    // Only a device node can be linked to. "rpool/home", "server:/export" and
    // "tmpfs" name no file, and are the fallback's to handle.
    const QString target = QFileInfo(device).canonicalFilePath();
    if (target.isEmpty())
        return {};

    DiskIdentity identity;
    identity.uuid = linkTo(diskDirectory + QStringLiteral("/by-uuid"), target);
    identity.label = decodeDiskLabel(linkTo(diskDirectory + QStringLiteral("/by-label"), target));
    return identity;
}

QString decodeDiskLabel(const QString& linkName)
{
    QByteArray out;
    const QByteArray in = linkName.toUtf8();
    for (qsizetype i = 0; i < in.size(); ++i) {
        if (in.at(i) == '\\' && i + 3 < in.size() && in.at(i + 1) == 'x') {
            bool ok = false;
            const int value = in.mid(i + 2, 2).toInt(&ok, 16);
            if (ok) {
                out.append(char(value));
                i += 3;
                continue;
            }
        }
        out.append(in.at(i));
    }
    return QString::fromUtf8(out);
}

} // namespace mole
