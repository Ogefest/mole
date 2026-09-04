#include "ui/models/DuplicateGroupModel.h"

#include "core/text/SizeWords.h"

namespace mole {

DuplicateGroupModel::DuplicateGroupModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int DuplicateGroupModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_groups.size());
}

QHash<int, QByteArray> DuplicateGroupModel::roleNames() const
{
    return { { CopiesRole, QByteArrayLiteral("copies") }, { SizeTextRole, QByteArrayLiteral("sizeText") },
        { ReclaimableTextRole, QByteArrayLiteral("reclaimableText") },
        { FilesRole, QByteArrayLiteral("files") } };
}

QVariant DuplicateGroupModel::data(const QModelIndex& index, int role) const
{
    if (index.row() < 0 || index.row() >= m_groups.size())
        return {};
    const DuplicateGroup& group = m_groups.at(index.row());

    switch (role) {
    case CopiesRole:
        return static_cast<int>(group.files.size());
    case SizeTextRole:
        return group.files.isEmpty() ? QString {} : sizeInWords(group.files.first().size);
    case ReclaimableTextRole:
        return sizeInWords(group.reclaimable);
    case FilesRole: {
        // Built here rather than kept, and therefore built for the rows a view
        // has on screen instead of for every file in the result every time
        // anything changes. That difference is the fault this class fixes.
        QVariantList files;
        files.reserve(group.files.size());
        for (const FileEntry& entry : group.files) {
            const QString uri = entry.uri.toString();
            files.append(QVariantMap { { QStringLiteral("uri"), uri }, { QStringLiteral("name"), entry.name },
                { QStringLiteral("location"), entry.uri.parent().toString() },
                { QStringLiteral("sizeText"), sizeInWords(entry.size) },
                { QStringLiteral("modifiedText"),
                    entry.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm")) },
                { QStringLiteral("selected"), m_selected.contains(uri) } });
        }
        return files;
    }
    default:
        return {};
    }
}

void DuplicateGroupModel::insertGroup(const DuplicateGroup& group, int position)
{
    const int at = qBound(0, position, static_cast<int>(m_groups.size()));
    beginInsertRows(QModelIndex(), at, at);
    m_groups.insert(at, group);
    m_reclaimable += group.reclaimable;
    m_copyCount += static_cast<int>(group.files.size());
    endInsertRows();
    emit countChanged();
}

void DuplicateGroupModel::clear()
{
    if (m_groups.isEmpty() && m_selected.isEmpty())
        return;

    beginResetModel();
    m_groups.clear();
    m_selected.clear();
    m_reclaimable = 0;
    m_copyCount = 0;
    m_selectedBytes = 0;
    endResetModel();
    emit countChanged();
}

void DuplicateGroupModel::removeUris(const QStringList& uris)
{
    if (uris.isEmpty() || m_groups.isEmpty())
        return;

    const QSet<QString> going(uris.begin(), uris.end());
    // Back to front, so a removal does not move the rows still to be examined.
    for (int row = static_cast<int>(m_groups.size()) - 1; row >= 0; --row) {
        QList<FileEntry> kept;
        for (const FileEntry& entry : m_groups.at(row).files) {
            if (!going.contains(entry.uri.toString()))
                kept.append(entry);
        }
        if (kept.size() == m_groups.at(row).files.size())
            continue;

        // One copy left is not a group. Neither is none, which is what deleting
        // every copy of something leaves -- and somebody who ticked every copy
        // of a file meant it.
        if (kept.size() < 2) {
            beginRemoveRows(QModelIndex(), row, row);
            m_groups.removeAt(row);
            endRemoveRows();
            continue;
        }

        DuplicateGroup& group = m_groups[row];
        group.files = kept;
        // What is left to reclaim, worked out again rather than left as it was:
        // it is "all but one of these", and there are fewer of them now.
        group.reclaimable = static_cast<qint64>(kept.size() - 1) * kept.first().size;
        emit dataChanged(index(row), index(row), { CopiesRole, ReclaimableTextRole, FilesRole });
    }

    for (const QString& uri : uris)
        m_selected.remove(uri);

    m_reclaimable = 0;
    m_copyCount = 0;
    for (const DuplicateGroup& group : std::as_const(m_groups)) {
        m_reclaimable += group.reclaimable;
        m_copyCount += static_cast<int>(group.files.size());
    }
    recountSelectedBytes();
    emit countChanged();
}

QStringList DuplicateGroupModel::selectedUris() const
{
    QStringList out = m_selected.values();
    out.sort();
    return out;
}

int DuplicateGroupModel::rowOf(const QString& uri) const
{
    for (int row = 0; row < m_groups.size(); ++row) {
        for (const FileEntry& entry : m_groups.at(row).files) {
            if (entry.uri.toString() == uri)
                return row;
        }
    }
    return -1;
}

void DuplicateGroupModel::toggle(const QString& uri)
{
    const int row = rowOf(uri);
    if (row < 0)
        return;

    qint64 size = 0;
    for (const FileEntry& entry : m_groups.at(row).files) {
        if (entry.uri.toString() == uri)
            size = entry.size;
    }

    if (m_selected.remove(uri))
        m_selectedBytes -= size;
    else {
        m_selected.insert(uri);
        m_selectedBytes += size;
    }

    const QModelIndex changed = index(row);
    emit dataChanged(changed, changed, { FilesRole });
}

void DuplicateGroupModel::keepOnly(const QString& uri)
{
    const int row = rowOf(uri);
    if (row < 0)
        return;

    for (const FileEntry& entry : m_groups.at(row).files) {
        const QString each = entry.uri.toString();
        if (each == uri)
            m_selected.remove(each);
        else
            m_selected.insert(each);
    }
    recountSelectedBytes();

    const QModelIndex changed = index(row);
    emit dataChanged(changed, changed, { FilesRole });
}

void DuplicateGroupModel::selectAllBut(const std::function<int(const QList<FileEntry>&)>& chooseKeeper)
{
    m_selected.clear();
    for (const DuplicateGroup& group : std::as_const(m_groups)) {
        const int keeper = chooseKeeper(group.files);
        for (int i = 0; i < group.files.size(); ++i) {
            if (i != keeper)
                m_selected.insert(group.files.at(i).uri.toString());
        }
    }
    recountSelectedBytes();
    announceEveryRow();
}

void DuplicateGroupModel::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    m_selectedBytes = 0;
    announceEveryRow();
}

void DuplicateGroupModel::announceEveryRow()
{
    if (m_groups.isEmpty())
        return;
    // One signal for the lot. Every row's ticks changed, but the rows themselves
    // did not move, so this is still an update rather than a replacement: the
    // delegates are told to re-read and nothing is destroyed.
    emit dataChanged(index(0), index(static_cast<int>(m_groups.size()) - 1), { FilesRole });
}

void DuplicateGroupModel::recountSelectedBytes()
{
    m_selectedBytes = 0;
    for (const DuplicateGroup& group : std::as_const(m_groups)) {
        for (const FileEntry& entry : group.files) {
            if (m_selected.contains(entry.uri.toString()))
                m_selectedBytes += entry.size;
        }
    }
}

} // namespace mole
