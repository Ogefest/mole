#pragma once

#include "sdk/PluginServices.h"

#include "core/vfs/VfsUri.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <memory>

class QMimeData;

namespace mole {

/// Turns a selection into something the rest of the desktop can take.
///
/// Everything else Mole does with a file it does inside its own window. This is
/// the one place that hands one out: a browser's upload box, another file
/// manager, a chat window. What leaves is a `text/uri-list` of paths that exist
/// on disk, and it always leaves as a copy -- see
/// [ADR-0040](../../docs/adr/0040-what-leaves-the-window-is-a-path-and-it-leaves-as-a-copy.md).
///
/// A row inside an archive or on a network drive has no such path, so it is given
/// one: the bytes are streamed into a scratch directory and that copy is what
/// leaves. **A drag cannot wait for it.** The gesture is over long before a
/// hundred megabytes arrive and there is no way to start a `QDrag` after the
/// button is up, so the first drag of a row that is not on disk starts the fetch
/// and says so, and the next one carries it.
class DragSource : public QObject
{
    Q_OBJECT

public:
    /// The final "hand it to the platform" step, replaceable exactly as
    /// `FileLauncher::OpenHook` is, so the suite can assert what would have been
    /// dragged without a window, a pointer or a platform.
    ///
    /// The hook takes ownership of `mime`, the way `QDrag::setMimeData()` does.
    /// There is no default: a `QDrag` needs a real window as its source and a
    /// platform to run its event loop on, and constructing one here would take
    /// `src/ui` -- and with it every test in `tests/ui` -- off the headless path
    /// it runs on today. The shell installs the real one.
    using StartHook = std::function<bool(std::unique_ptr<QMimeData> mime, Qt::DropActions actions)>;

    explicit DragSource(PluginServices services, QObject* parent = nullptr);
    ~DragSource() override;

    void setStartHook(StartHook hook);

    /// Hands `rows` to the desktop, in the order they were given. Rows that are
    /// not on disk are fetched instead, and nothing is dragged that time.
    void start(const QList<VfsUri>& rows);

signals:
    /// Handed over: `count` rows went, as a copy.
    void started(int count);
    /// Nothing went, and why. A gesture that quietly does nothing reads as a
    /// broken pointer rather than as a refusal, so there is always a reason.
    void refused(const QString& reason);
    /// The drag went with `sent` rows and `left` rows stayed behind because no
    /// drive is mounted for them. Half a selection leaving silently is the one
    /// outcome this must not have.
    void leftBehind(int sent, int left);
    /// `count` rows are being fetched into a scratch copy. Nothing is being
    /// dragged this time, and the same drag will carry them once they are here.
    void staging(int count);

private:
    /// Where `uri` is staged. The original name is kept so the receiver picks a
    /// handler by extension, and the parent path is kept as a subdirectory so two
    /// files called `readme.txt` from different folders do not collide -- the same
    /// scheme `FileLauncher` stages a remote file it has been asked to open under.
    QString stagedPathFor(const VfsUri& uri);
    /// Whether the copy already staged for `source` can be handed over as it is.
    bool stagedCopyIsFresh(const VfsUri& source) const;
    /// Queues the fetch for `rows` and says so. Nothing is dragged.
    void stage(const QList<VfsUri>& rows);
    /// Notes what a source looked like at the moment its copy was made, so a
    /// later drag can tell whether the copy is still the file.
    void remember(const VfsUri& source, const QString& stagedPath);

    /// What was staged for one source, and what the source was when it was.
    struct Staged
    {
        QString path;
        /// -1 for a directory, which is reused as it stands: deciding whether a
        /// tree is still the same tree means walking it, and that costs what
        /// fetching it again costs.
        qint64 size = -1;
        QDateTime modified;
    };

    PluginServices m_services;
    StartHook m_startHook;
    std::unique_ptr<QTemporaryDir> m_scratch;
    QHash<QString, Staged> m_staged;
};

} // namespace mole
