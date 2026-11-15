#include "core/vfs/MountTableWatch.h"

#ifdef Q_OS_LINUX
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mole {

namespace {

#ifdef Q_OS_LINUX
    /// The whole table, read from the start through the descriptor the notifier
    /// holds. Reading it through that descriptor is what re-arms POLLPRI, so it
    /// cannot be a fresh open of the file each time.
    QByteArray readFromStart(int descriptor)
    {
        if (::lseek(descriptor, 0, SEEK_SET) < 0)
            return {};
        QByteArray text;
        char buffer[8192];
        for (;;) {
            const ssize_t got = ::read(descriptor, buffer, sizeof buffer);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            text.append(buffer, static_cast<qsizetype>(got));
        }
        return text;
    }
#endif

} // namespace

MountTableWatch::MountTableWatch(HostPlatform platform, QObject* parent)
    : QObject(parent)
{
    if (platform != HostPlatform::Posix) {
        // No table to poll, and a timer over a statvfs per mount is MOLE-361 on a
        // schedule. See the class comment and ADR-0104.
        m_whyNot
            = QStringLiteral("this platform has no mount table to watch; drives are read once, at startup");
        return;
    }
#ifdef Q_OS_LINUX
    const int descriptor = ::open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        m_whyNot = QStringLiteral("/proc/self/mounts could not be opened; drives are read once, at startup");
        return;
    }
    Source source;
    source.descriptor = descriptor;
    source.wakesOn = QSocketNotifier::Exception;
    source.read = [descriptor] { return readFromStart(descriptor); };
    source.close = [descriptor] { ::close(descriptor); };
    start(std::move(source));
#else
    // A Posix system that is not Linux -- a BSD -- has no /proc/self/mounts.
    m_whyNot = QStringLiteral("this system has no pollable mount table; drives are read once, at startup");
#endif
}

MountTableWatch::MountTableWatch(Source source, QObject* parent)
    : QObject(parent)
{
    start(std::move(source));
}

MountTableWatch::~MountTableWatch()
{
    // The notifier goes before the descriptor it is watching.
    delete m_notifier;
    m_notifier = nullptr;
    if (m_source.close)
        m_source.close();
}

void MountTableWatch::start(Source source)
{
    m_source = std::move(source);
    if (m_source.descriptor < 0 || !m_source.read) {
        m_whyNot = QStringLiteral("no mount table was given to watch");
        return;
    }
    // Read once now: the first wake is compared against this, and for the real
    // table a read is what arms the notification in the first place.
    m_last = m_source.read();
    ++m_reads;
    m_notifier = new QSocketNotifier(static_cast<qintptr>(m_source.descriptor), m_source.wakesOn, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &MountTableWatch::woken);
}

void MountTableWatch::woken()
{
    const QByteArray now = m_source.read();
    ++m_reads;
    if (now == m_last)
        return;
    m_last = now;
    emit changed();
}

} // namespace mole
