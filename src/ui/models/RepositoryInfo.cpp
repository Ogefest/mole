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

    m_present = true;
    m_root = root;
    m_head = head;
    emit changed();
}

void RepositoryInfo::clear()
{
    if (!m_present && m_root.isEmpty())
        return;
    m_present = false;
    m_root.clear();
    m_head = RepositoryHead {};
    emit changed();
}

} // namespace mole
