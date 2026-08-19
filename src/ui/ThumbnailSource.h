#pragma once

#include "sdk/PluginServices.h"

#include "core/tasks/Task.h"
#include "core/vfs/VfsUri.h"

#include <QHash>
#include <QImage>
#include <QPointer>

namespace mole {

/// The three things a thumbnail is keyed on, and the url that carries them.
///
/// `image://mole-thumb/<percent-encoded uri>?size=160&mtime=<seconds since epoch>`
///
/// The modification time is in the url because it is what makes an edited file
/// produce a new picture rather than the old one -- and it arrives free from a
/// role the listing already holds, so keying costs no extra `stat()`. The size is
/// in it because the same file in a small grid and in the gallery are two
/// different pictures.
struct ThumbnailKey
{
    VfsUri uri;
    int size = 0;
    /// Seconds since the epoch, or 0 when the drive does not date its files --
    /// which means the picture cannot be invalidated by a change and will be
    /// remade when the size changes and not before.
    qint64 mtime = 0;

    bool isValid() const { return uri.isValid() && size > 0; }

    /// The id Qt hands an image provider: everything after `image://mole-thumb/`.
    QString toId() const;
    /// The reverse. An invalid key back for anything that is not one of ours,
    /// which is what a mistyped url in a view looks like from here.
    static ThumbnailKey parse(const QString& id);

    /// The provider name the url addresses.
    static QString providerName() { return QStringLiteral("mole-thumb"); }
    /// The whole url, for a view that wants to build one in a binding.
    static QString urlFor(const VfsUri& uri, int size, qint64 mtime);
};

/// Makes one thumbnail, on a worker thread.
///
/// The registry decides who answers; this owns the running of it, the
/// cancellation and the answer. A null image is an ordinary result: no
/// thumbnailer claimed the file, or the one that did could not read it.
class ThumbnailTask final : public Task
{
    Q_OBJECT

public:
    ThumbnailTask(PluginServices services, ThumbnailKey key, QObject* parent = nullptr);

    /// Valid only once the task has finished. Null means "no thumbnail", which
    /// every caller has to treat as an answer rather than as a failure.
    QImage image() const { return m_image; }
    /// Which thumbnailer answered, empty when none claimed the file. Here for a
    /// test that wants to know priority was honoured.
    QString answeredBy() const { return m_answeredBy; }
    /// The thread run() executed on, so a test can hold the house rule that
    /// nothing decodes on the GUI thread.
    QThread* ranOn() const { return m_ranOn; }

protected:
    void run() override;

private:
    PluginServices m_services;
    ThumbnailKey m_key;
    QImage m_image;
    QString m_answeredBy;
    QThread* m_ranOn = nullptr;
};

/// Runs thumbnail requests for whoever asks, from whatever thread they ask on.
///
/// It exists because a Qt Quick image provider may be called from the pixmap
/// reader thread while `TaskManager::submit()` has to be called from the thread
/// that owns it. So a request is posted here, this object lives on the UI thread,
/// and everything about the task -- creating it, submitting it, cancelling it,
/// reading its answer -- happens there. The Quick types stay in the shell; this
/// is the half that can be tested headlessly.
class ThumbnailPump : public QObject
{
    Q_OBJECT

public:
    explicit ThumbnailPump(PluginServices services, QObject* parent = nullptr);

    /// How many requests are being worked on right now. For a test, and for the
    /// scheduling MOLE-142 adds.
    int outstanding() const { return int(m_running.size()); }

public slots:
    /// Starts work for `id` on behalf of `response`, which is only ever used as
    /// an identity: it is never dereferenced here, so a response destroyed in
    /// the meantime cannot be followed.
    void startFor(QObject* response, const QString& id);
    /// Stops the work for `response` if it is still running. The answer still
    /// arrives -- Qt expects a cancelled response to finish -- and it is null.
    void cancelFor(QObject* response);

signals:
    /// The answer, always on the UI thread and always exactly once per accepted
    /// request. A null image means there is no thumbnail, which is ordinary.
    void ready(QObject* response, const QImage& image);

private:
    void deliver(QObject* response, const QImage& image);

    PluginServices m_services;
    /// Response identity to the task making its picture. Touched only on the UI
    /// thread, which is what makes an identity comparison against a pointer that
    /// may already be gone safe: it is never dereferenced.
    QHash<QObject*, QPointer<ThumbnailTask>> m_running;
};

} // namespace mole
