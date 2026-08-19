#pragma once

#include "sdk/PluginServices.h"

#include "core/tasks/Task.h"
#include "core/vfs/VfsUri.h"

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QStringList>

namespace mole {

class ThumbnailCache;

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
    /// `cache` is borrowed and may be null, which is a run with no cache at all.
    ThumbnailTask(
        PluginServices services, ThumbnailKey key, ThumbnailCache* cache, QObject* parent = nullptr);

    /// Valid only once the task has finished. Null means "no thumbnail", which
    /// every caller has to treat as an answer rather than as a failure.
    QImage image() const { return m_image; }
    /// Which thumbnailer answered, empty when none claimed the file. Here for a
    /// test that wants to know priority was honoured.
    QString answeredBy() const { return m_answeredBy; }
    /// The thread run() executed on, so a test can hold the house rule that
    /// nothing decodes on the GUI thread.
    QThread* ranOn() const { return m_ranOn; }
    /// True when the answer came out of the disk cache rather than a decode. What
    /// a test counts to hold that a second visit decodes nothing.
    bool cameFromCache() const { return m_fromCache; }

protected:
    void run() override;

private:
    PluginServices m_services;
    ThumbnailKey m_key;
    ThumbnailCache* m_cache = nullptr;
    QImage m_image;
    QString m_answeredBy;
    QThread* m_ranOn = nullptr;
    bool m_fromCache = false;
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
    /// `cache` is borrowed and outlives the pump; null means every request is a
    /// fresh decode, which is what a test that counts decodes usually wants.
    explicit ThumbnailPump(PluginServices services, ThumbnailCache* cache, QObject* parent = nullptr);

    /// How many decodes are being worked on right now. Two panes asking for the
    /// same picture are one, which is the point of counting keys rather than
    /// requests. For a test, and for the scheduling MOLE-142 adds.
    int outstanding() const { return int(m_pending.size()); }
    /// How many requests are waiting for an answer, which is more than
    /// outstanding() when two of them want the same picture.
    int waiting() const { return int(m_keyOf.size()); }

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

    /// One decode, and everybody waiting for it. Two panes showing the same folder
    /// ask for the same picture at the same moment, and decoding it twice is twice
    /// the work for one answer.
    struct Pending
    {
        QPointer<ThumbnailTask> task;
        QList<QObject*> waiting;
    };

    PluginServices m_services;
    ThumbnailCache* m_cache = nullptr;
    /// Key to the decode making its picture, and who is waiting.
    QHash<QString, Pending> m_pending;
    /// Which key a response is waiting on. Touched only on the UI thread, which is
    /// what makes an identity comparison against a pointer that may already be
    /// gone safe: it is never dereferenced.
    QHash<QObject*, QString> m_keyOf;
};

} // namespace mole
