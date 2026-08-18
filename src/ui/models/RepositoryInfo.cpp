#include "ui/models/RepositoryInfo.h"

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

QString RepositoryInfo::changesText() const
{
    if (!m_present || !m_statusKnown)
        return {};
    if (m_status.changedCount == 0)
        return QStringLiteral("clean");
    return QStringLiteral("%1 changed").arg(m_status.changedCount);
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
    emit changed();
}

void RepositoryInfo::clearStatus()
{
    if (!m_statusKnown && m_status.byPath.isEmpty())
        return;
    m_statusKnown = false;
    m_status = RepositoryStatus {};
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
    emit changed();
}

} // namespace mole
