#include "core/sets/VerifySetTask.h"

#include "core/vfs/VfsManager.h"

namespace mole {

VerifySetTask::VerifySetTask(VfsManager* vfs, QStringList uris, QObject* parent)
    : Task(QStringLiteral("Check %1 items").arg(uris.size()), parent)
    , m_vfs(vfs)
    , m_uris(std::move(uris))
{
    setBackground(true);
}

void VerifySetTask::run()
{
    if (!m_vfs) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("No drives are available")));
        return;
    }

    QHash<QString, bool> present;
    QHash<QString, qint64> sizes;
    int missing = 0;
    int done = 0;

    for (const QString& uri : std::as_const(m_uris)) {
        if (isCancelRequested())
            return;

        const VfsUri parsed = VfsUri::fromString(uri);
        FileSystemPtr fs = parsed.isValid() ? m_vfs->resolve(parsed) : nullptr;

        if (!fs) {
            // A drive that is not mounted is not the same as a file that is
            // gone, but from the set's point of view it is equally unusable --
            // and saying so beats silently treating it as fine.
            present.insert(uri, false);
            ++missing;
        } else if (Result<FileEntry> stat = fs->stat(parsed); stat.ok()) {
            present.insert(uri, true);
            if (!stat.value().isDir)
                sizes.insert(uri, stat.value().size);
        } else {
            present.insert(uri, false);
            ++missing;
        }

        setProgress(static_cast<int>(100.0 * ++done / m_uris.size()));
        reportCount(QStringLiteral("checked"), QStringLiteral("Checked"), done, 10);
        if (missing > 0)
            reportCount(QStringLiteral("missing"), QStringLiteral("Missing"), missing, 20);
    }

    setStatusText(missing == 0 ? QStringLiteral("all present") : QStringLiteral("%1 missing").arg(missing));
    emit verified(present, sizes);
}

} // namespace mole
