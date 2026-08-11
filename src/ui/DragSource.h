#pragma once

#include "core/vfs/VfsUri.h"

#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

class QMimeData;

namespace mole {

/// Turns a selection into something the rest of the desktop can take.
///
/// Everything else Mole does with a file it does inside its own window. This is
/// the one place that hands one out: a browser's upload box, another file
/// manager, a chat window. What leaves is a `text/uri-list` of paths that
/// already exist on disk, and it always leaves as a copy -- see
/// [ADR-0040](../../docs/adr/0040-what-leaves-the-window-is-a-path-and-it-leaves-as-a-copy.md).
///
/// Rows that have no path on disk -- anything on an archive or a network drive
/// -- are left out, because a `text/uri-list` names a file the receiving
/// application opens for itself and there is nothing there to open. Giving those
/// rows a path of their own is a later step; until then they are reported rather
/// than dropped in silence.
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

    explicit DragSource(QObject* parent = nullptr);
    ~DragSource() override;

    void setStartHook(StartHook hook);

    /// Hands `rows` to the desktop, in the order they were given.
    void start(const QList<VfsUri>& rows);

signals:
    /// Handed over: `count` rows went, as a copy.
    void started(int count);
    /// Nothing went, and why. A gesture that quietly does nothing reads as a
    /// broken pointer rather than as a refusal, so there is always a reason.
    void refused(const QString& reason);
    /// The drag went with `sent` rows and `left` rows stayed behind because they
    /// have no path on disk. Half a selection leaving silently is the one
    /// outcome this must not have.
    void leftBehind(int sent, int left);

private:
    StartHook m_startHook;
};

} // namespace mole
