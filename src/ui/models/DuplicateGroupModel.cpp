#include "ui/models/DuplicateGroupModel.h"

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
        return group.files.isEmpty() ? QString {} : m_locale.formattedDataSize(group.files.first().size);
    case ReclaimableTextRole:
        return m_locale.formattedDataSize(group.reclaimable);
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
                { QStringLiteral("sizeText"), m_locale.formattedDataSize(entry.size) },
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
