#include "ui/models/RepositoryInfo.h"

#include "core/vfs/VfsUri.h"

#include <QStringList>
#include <QVariantMap>

#include <algorithm>

namespace mole {

RepositoryInfo::RepositoryInfo(QObject* parent)
    : QObject(parent)
{
}

QString RepositoryInfo::headText() const
{
    if (!m_present)
        return {};
    const QString state = m_head.stateText();
    if (!state.isEmpty())
        return state;
    if (m_head.detached) {
        return m_head.shortId.isEmpty() ? QStringLiteral("detached")
                                        : QStringLiteral("detached at %1").arg(m_head.shortId);
    }
    return m_head.branch;
}

namespace {

    /// "12 min ago", "3 h ago", "yesterday", "40 days ago".
    ///
    /// The same shape the reports list and the drive rows use. It is spelled out in
    /// each of those places too; the fourth copy is the one that should have been an
    /// extraction, and TODO.md says so rather than pretending otherwise.
    QString relativeTime(const QDateTime& when)
    {
        if (!when.isValid())
            return {};
        const qint64 seconds = when.secsTo(QDateTime::currentDateTime());
        if (seconds < 0)
            return QStringLiteral("just now");
        if (seconds < 3600)
            return QStringLiteral("%1 min ago").arg(std::max<qint64>(1, seconds / 60));
        if (seconds < 86400)
            return QStringLiteral("%1 h ago").arg(seconds / 3600);
        const qint64 days = seconds / 86400;
        return days == 1 ? QStringLiteral("yesterday") : QStringLiteral("%1 days ago").arg(days);
    }

} // namespace

QString RepositoryInfo::trackingText() const
{
    if (!m_present || !m_head.hasUpstream)
        return {};

    // Nothing when the two agree. "0 ahead, 0 behind" is a sentence about arithmetic,
    // and there is no decision for anybody to take from it.
    QStringList parts;
    if (m_head.ahead > 0)
        parts.append(QStringLiteral("%1 ahead").arg(m_head.ahead));
    if (m_head.behind > 0)
        parts.append(QStringLiteral("%1 behind").arg(m_head.behind));
    return parts.join(QStringLiteral(", "));
}

QString RepositoryInfo::commitAge() const
{
    return hasCommit() ? relativeTime(m_head.committedAt) : QString {};
}

QString RepositoryInfo::changesText() const
{
    if (!m_present || !m_statusKnown)
        return {};
    if (m_status.changedCount == 0)
        return QStringLiteral("clean");
    return QStringLiteral("%1 changed").arg(m_status.changedCount);
}

void RepositoryInfo::rebuildChangedPaths()
{
    m_changedPaths.clear();
    if (!m_statusKnown || m_root.isEmpty())
        return;

    // The absolute paths first, so the sort is over what the hash is keyed by and
    // the relative name is cut once per entry rather than once per comparison.
    QStringList absolute;
    absolute.reserve(m_status.byPath.size());
    for (auto it = m_status.byPath.cbegin(); it != m_status.byPath.cend(); ++it) {
        // A directory this walk rolled up carries nothing else, and is not a path
        // git reported. Left out rather than shown as a bullet: every one of them
        // is a parent of something already in the list.
        if ((it.value() & RepositoryReportedStates) != 0)
            absolute.append(it.key());
    }
    // A QHash iterates in whatever order it feels like, so the list would
    // otherwise be different on two runs over the same checkout.
    absolute.sort();

    const int cut = m_root.length() + 1;
    m_changedPaths.reserve(absolute.size());
    for (const QString& path : std::as_const(absolute)) {
        const int state = m_status.byPath.value(path, RepositoryUnchanged);
        QVariantMap entry;
        entry.insert(QStringLiteral("path"), path.mid(cut));
        entry.insert(QStringLiteral("mark"), repositoryStateMark(state));
        entry.insert(QStringLiteral("uri"), VfsUri::fromLocalPath(path).toString());
        // The one entry with nowhere to go. Reported rather than left for the
        // band to work out from the mark, because a file may be deleted and
        // something else at once and the mark shows only the most urgent of them.
        entry.insert(QStringLiteral("deleted"), (state & RepositoryDeleted) != 0);
        m_changedPaths.append(entry);
    }
}

void RepositoryInfo::setStatus(const QString& root, const RepositoryStatus& status)
{
    // An answer about the checkout somebody has already navigated out of. The
    // walk is the slow half of this feature, so this really happens.
    if (!m_present || root.isEmpty() || root != m_root)
        return;
    if (!status.complete)
        return;
    if (m_statusKnown && m_status.changedCount == status.changedCount && m_status.byPath == status.byPath) {
        return;
    }

    m_statusKnown = true;
    m_status = status;
    rebuildChangedPaths();
    emit changed();
}

void RepositoryInfo::clearStatus()
{
    if (!m_statusKnown && m_status.byPath.isEmpty())
        return;
    m_statusKnown = false;
    m_status = RepositoryStatus {};
    m_changedPaths.clear();
    emit changed();
}

void RepositoryInfo::setHead(const QString& root, const RepositoryHead& head)
{
    if (root.isEmpty()) {
        clear();
        return;
    }

    // A repository that could be opened but says nothing about HEAD is a
    // repository nobody can name: no branch, not detached, not unborn. Rather
    // than a band with an empty label, there is no band.
    if (!head.isValid()) {
        clear();
        return;
    }

    if (m_present && m_root == root && m_head.branch == head.branch && m_head.shortId == head.shortId
        && m_head.detached == head.detached && m_head.unborn == head.unborn && m_head.state == head.state) {
        return;
    }

    // A different checkout is a different answer. Dropped rather than kept while
    // the new walk runs, because one repository's count beside another's branch is
    // a wrong answer, and a missing one is not.
    if (m_root != root) {
        m_statusKnown = false;
        m_status = RepositoryStatus {};
        m_changedPaths.clear();
    }

    m_present = true;
    m_root = root;
    m_head = head;
    emit changed();
}

void RepositoryInfo::clear()
{
    if (!m_present && m_root.isEmpty() && !m_statusKnown)
        return;
    m_present = false;
    m_root.clear();
    m_head = RepositoryHead {};
    m_statusKnown = false;
    m_status = RepositoryStatus {};
    m_changedPaths.clear();
    emit changed();
}

} // namespace mole
