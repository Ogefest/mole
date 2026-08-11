#include "ui/DragSource.h"

#include <QMimeData>
#include <QUrl>

namespace mole {

DragSource::DragSource(QObject* parent)
    : QObject(parent)
{
}

DragSource::~DragSource() = default;

void DragSource::setStartHook(StartHook hook)
{
    m_startHook = hook ? std::move(hook) : StartHook();
}

void DragSource::start(const QList<VfsUri>& rows)
{
    if (rows.isEmpty()) {
        emit refused(QStringLiteral("Nothing is selected"));
        return;
    }

    QList<QUrl> urls;
    int left = 0;
    for (const VfsUri& row : rows) {
        // A directory goes out as its own url. Expanding it here would hand the
        // receiver a flat list of leaves and lose the folder it was dragged as.
        const QString localPath = row.toLocalPath();
        if (localPath.isEmpty()) {
            ++left;
            continue;
        }
        urls.append(QUrl::fromLocalFile(localPath));
    }

    if (urls.isEmpty()) {
        emit refused(QStringLiteral("A file on an archive or a network drive has no path another application "
                                    "can open"));
        return;
    }

    if (!m_startHook) {
        emit refused(QStringLiteral("No handler is configured"));
        return;
    }

    auto mime = std::make_unique<QMimeData>();
    mime->setUrls(urls);

    // Copy and nothing else. A receiver that asked for a move would otherwise
    // delete the source on the strength of a gesture that looks exactly like the
    // one that copies -- and nothing ever leaves Mole by being moved.
    if (!m_startHook(std::move(mime), Qt::CopyAction)) {
        emit refused(QStringLiteral("The desktop did not take the files"));
        return;
    }

    emit started(static_cast<int>(urls.size()));
    if (left > 0)
        emit leftBehind(static_cast<int>(urls.size()), left);
}

} // namespace mole
