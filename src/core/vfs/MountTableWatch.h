#pragma once

#include "core/platform/HostPlatform.h"

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>

#include <functional>

namespace mole {

/// Says when the operating system's mount table has changed, and nothing else.
///
/// What is mounted used to be asked once, at startup, so a drive plugged in
/// afterwards did not exist as far as Mole was concerned. This is the piece that
/// notices; what to do about it -- the diff against the drive list -- belongs to
/// whoever listens. It knows nothing about drives or the sidebar. See MOLE-428 and
/// ADR-0104.
///
/// **A watch, not a timer.** On Linux /proc/self/mounts is pollable: it raises an
/// exceptional condition (POLLPRI) when the table changes and stays quiet when it
/// does not, and reading it again re-arms it. So while nothing is mounted or
/// unmounted this costs nothing at all -- nothing is read and no signal is sent.
/// A wake whose table reads the same as before (a mount and its unmount between
/// two reads) is not passed on either.
///
/// **Elsewhere it does not watch.** macOS and Windows have no table to poll, and
/// what SystemVolumes reads there instead is a statvfs per mount: a timer over
/// that would bring back MOLE-361, a stale share blocking for the kernel's
/// timeout, on a schedule. The real answer there is a platform notification, and
/// that waits for a machine to measure it on. Until then isWatching() is false,
/// whyNotWatching() says so, and nothing is started.
class MountTableWatch : public QObject
{
    Q_OBJECT

public:
    /// Where the table comes from, and how its descriptor says it changed.
    ///
    /// A seam for tests, in the manner of SystemVolumes::setEnumerator(): a test
    /// hands it the read end of a pipe and a reader of its own, and drives a real
    /// notifier without needing a mount namespace or a real mount.
    struct Source
    {
        /// A descriptor the notifier waits on. Owned by whoever made the Source;
        /// closed by `close` if one is given.
        int descriptor = -1;
        /// Exception for the mount table, which signals with POLLPRI.
        QSocketNotifier::Type wakesOn = QSocketNotifier::Exception;
        /// Reads the whole table. Called once at the start, for the text to
        /// compare against, and once per wake -- which, for the real table, is
        /// also what re-arms the notification.
        std::function<QByteArray()> read;
        /// Called when the watch is destroyed.
        std::function<void()> close;
    };

    /// Watches the real mount table on `platform`, if that platform has one.
    explicit MountTableWatch(HostPlatform platform = hostPlatform(), QObject* parent = nullptr);
    /// Watches `source` instead.
    explicit MountTableWatch(Source source, QObject* parent = nullptr);
    ~MountTableWatch() override;

    bool isWatching() const { return m_notifier != nullptr; }
    /// Empty while watching; otherwise one sentence for the log.
    QString whyNotWatching() const { return m_whyNot; }

    /// How many times the table has been read since the start, the first read
    /// included. For the claim that an unchanged table costs nothing.
    int readCount() const { return m_reads; }

signals:
    /// The table reads differently from the last time it was read.
    void changed();

private:
    void start(Source source);
    void woken();

    Source m_source;
    QSocketNotifier* m_notifier = nullptr;
    QByteArray m_last;
    QString m_whyNot;
    int m_reads = 0;
};

} // namespace mole
