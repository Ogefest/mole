#include "ui/models/FileListModel.h"

#include <QCollator>
#include <QLocale>

#include <algorithm>

namespace mole {
namespace {

    /// Natural ordering, so "file10" sorts after "file9" the way a person
    /// expects rather than the way strcmp does.
    const QCollator& nameCollator()
    {
        static const QCollator collator = [] {
            QCollator c;
            c.setNumericMode(true);
            c.setCaseSensitivity(Qt::CaseInsensitive);
            return c;
        }();
        return collator;
    }

    QString iconFor(const FileEntry& entry)
    {
        if (entry.isDir)
            return QStringLiteral("\U0001F4C1");

        static const QHash<QString, QString> bySuffix {
            { QStringLiteral("pdf"), QStringLiteral("\U0001F4D5") },
            { QStringLiteral("txt"), QStringLiteral("\U0001F4C4") },
            { QStringLiteral("md"), QStringLiteral("\U0001F4C4") },
            { QStringLiteral("mp3"), QStringLiteral("\U0001F3B5") },
            { QStringLiteral("flac"), QStringLiteral("\U0001F3B5") },
            { QStringLiteral("wav"), QStringLiteral("\U0001F3B5") },
            { QStringLiteral("png"), QStringLiteral("\U0001F5BC") },
            { QStringLiteral("jpg"), QStringLiteral("\U0001F5BC") },
            { QStringLiteral("jpeg"), QStringLiteral("\U0001F5BC") },
            { QStringLiteral("svg"), QStringLiteral("\U0001F5BC") },
            { QStringLiteral("mp4"), QStringLiteral("\U0001F3AC") },
            { QStringLiteral("mkv"), QStringLiteral("\U0001F3AC") },
            { QStringLiteral("zip"), QStringLiteral("\U0001F5DC") },
            { QStringLiteral("gz"), QStringLiteral("\U0001F5DC") },
            { QStringLiteral("tar"), QStringLiteral("\U0001F5DC") },
            { QStringLiteral("sqlite"), QStringLiteral("\U0001F5C3") },
            { QStringLiteral("db"), QStringLiteral("\U0001F5C3") },
            { QStringLiteral("duckdb"), QStringLiteral("\U0001F5C3") },
            { QStringLiteral("parquet"), QStringLiteral("\U0001F4CA") },
            { QStringLiteral("csv"), QStringLiteral("\U0001F4CA") },
            { QStringLiteral("xlsx"), QStringLiteral("\U0001F4CA") },
        };

        return bySuffix.value(entry.uri.suffix(), QStringLiteral("\U0001F4C4"));
    }

} // namespace

FileListModel::FileListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

QString FileListModel::formatSize(qint64 bytes)
{
    if (bytes < 0)
        return {};
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);

    static const QStringList units { QStringLiteral("kB"), QStringLiteral("MB"), QStringLiteral("GB"),
        QStringLiteral("TB"), QStringLiteral("PB") };

    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value < 10.0 ? 1 : 0).arg(units.at(unit));
}

void FileListModel::setEntries(FileEntryList entries)
{
    beginResetModel();
    m_all = std::move(entries);
    // A measurement describes the tree as it was when it was taken, so a new
    // listing starts without any.
    m_measured.clear();
    rebuildVisible();
    pruneSelection();
    endResetModel();
    emit countChanged();
    emit selectionChanged();
}

void FileListModel::appendEntries(const FileEntryList& entries)
{
    if (entries.isEmpty())
        return;

    // Sorting means a new batch can land anywhere, so this is a reset rather than
    // an append. What it must not be is a re-sort of everything found so far:
    // doing that on every batch is quadratic, on the thread that draws the window,
    // and it is what made a long search freeze the interface while a longer
    // analysis -- which never touches a visible model -- stayed smooth. Measured
    // at forty thousand results in batches of two hundred: 9.7 seconds re-sorting,
    // a fraction of a second merging.
    FileEntryList fresh;
    fresh.reserve(entries.size());
    for (const FileEntry& entry : entries) {
        if (entry.isHidden && !m_showHidden)
            continue;
        if (!m_filterFolded.isEmpty() && !entry.name.toLower().contains(m_filterFolded))
            continue;
        fresh.append(entry);
    }

    beginResetModel();
    m_all.append(entries);
    if (!fresh.isEmpty()) {
        const auto order = [this](const FileEntry& a, const FileEntry& b) { return lessThan(a, b); };
        std::stable_sort(fresh.begin(), fresh.end(), order);
        // Sort the batch, then merge it into what is already in order. Both halves
        // are sorted by the same comparator, so the result is too.
        const qsizetype pivot = m_visible.size();
        m_visible.append(fresh);
        std::inplace_merge(m_visible.begin(), m_visible.begin() + pivot, m_visible.end(), order);
    }
    endResetModel();
    emit countChanged();
}

void FileListModel::clear()
{
    if (m_all.isEmpty())
        return;
    beginResetModel();
    m_all.clear();
    m_visible.clear();
    m_selected.clear();
    m_measured.clear();
    endResetModel();
    emit countChanged();
    emit selectionChanged();
}

void FileListModel::setShowHidden(bool show)
{
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    beginResetModel();
    rebuildVisible();
    endResetModel();
    emit showHiddenChanged();
    emit countChanged();
}

void FileListModel::setFilterText(const QString& text)
{
    if (m_filterText == text)
        return;
    m_filterText = text;
    // Folded with Qt rather than SQLite's ASCII-only rules, so "Łódź" matches
    // "łódź" here exactly as it does in the index.
    m_filterFolded = text.toLower();

    beginResetModel();
    rebuildVisible();
    endResetModel();

    emit filterChanged();
    emit countChanged();
    emit selectionChanged();
}

void FileListModel::setSortKey(SortKey key)
{
    if (m_sortKey == key)
        return;
    m_sortKey = key;
    beginResetModel();
    rebuildVisible();
    endResetModel();
    emit sortChanged();
}

void FileListModel::setSortDescending(bool descending)
{
    if (m_sortDescending == descending)
        return;
    m_sortDescending = descending;
    beginResetModel();
    rebuildVisible();
    endResetModel();
    emit sortChanged();
}

bool FileListModel::lessThan(const FileEntry& a, const FileEntry& b) const
{
    // Directories always lead, regardless of sort direction. Flipping that
    // scatters folders through the list and nobody wants it.
    if (a.isDir != b.isDir)
        return a.isDir;

    int comparison = 0;
    switch (m_sortKey) {
    case SortKey::Size:
        comparison = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        break;
    case SortKey::Modified:
        comparison = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0);
        break;
    case SortKey::Type:
        comparison = nameCollator().compare(a.uri.suffix(), b.uri.suffix());
        break;
    case SortKey::Name:
        break;
    }

    if (comparison == 0)
        comparison = nameCollator().compare(a.name, b.name);

    return m_sortDescending ? comparison > 0 : comparison < 0;
}

void FileListModel::setAnnotations(QHash<QString, int> annotations)
{
    if (m_annotations == annotations)
        return;
    m_annotations = std::move(annotations);
    if (rowCount() > 0) {
        emit dataChanged(
            index(0, 0), index(rowCount() - 1, 0), { HasReportRole, HasAlertRole, AlertTriggeredRole });
    }
}

void FileListModel::rebuildVisible()
{
    m_visible.clear();
    m_visible.reserve(m_all.size());
    for (const FileEntry& entry : std::as_const(m_all)) {
        if (entry.isHidden && !m_showHidden)
            continue;
        if (!m_filterFolded.isEmpty() && !entry.name.toLower().contains(m_filterFolded))
            continue;
        m_visible.append(entry);
    }

    std::stable_sort(m_visible.begin(), m_visible.end(),
        [this](const FileEntry& a, const FileEntry& b) { return lessThan(a, b); });
}

int FileListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}

QVariant FileListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};

    const FileEntry& entry = m_visible.at(index.row());
    switch (role) {
    case NameRole:
    case Qt::DisplayRole:
        return entry.name;
    case UriRole:
        return entry.uri.toString();
    case ParentUriRole:
        return entry.uri.parent().toString();
    case IsDirRole:
        return entry.isDir;
    case IsHiddenRole:
        return entry.isHidden;
    case SizeRole:
        // A measured folder sorts by what is inside it, which is the only number
        // anyone means when they sort a listing by size.
        if (entry.isDir) {
            const qint64 measured = measuredSize(entry.uri.toString());
            return measured >= 0 ? measured : entry.size;
        }
        return entry.size;
    case SizeTextRole: {
        if (!entry.isDir)
            return formatSize(entry.size);
        const qint64 measured = measuredSize(entry.uri.toString());
        return measured >= 0 ? formatSize(measured) : QString();
    }
    case ModifiedRole:
        return entry.modified;
    case ModifiedTextRole:
        return entry.modified.isValid() ? QLocale().toString(entry.modified, QLocale::ShortFormat)
                                        : QString();
    case SuffixRole:
        return entry.uri.suffix();
    case IconTextRole:
        return iconFor(entry);
    case HasReportRole:
        return (annotationFor(entry.uri.toString()) & ReportPresent) != 0;
    case HasAlertRole:
        return (annotationFor(entry.uri.toString()) & AlertPresent) != 0;
    case AlertTriggeredRole:
        return (annotationFor(entry.uri.toString()) & AlertTriggered) != 0;
    case SelectedRole:
        return m_selected.contains(entry.uri.toString());
    default:
        return {};
    }
}

QHash<int, QByteArray> FileListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { UriRole, "uri" },
        { ParentUriRole, "parentUri" },
        { IsDirRole, "isDir" },
        { IsHiddenRole, "isHidden" },
        { SizeRole, "size" },
        { SizeTextRole, "sizeText" },
        { ModifiedRole, "modified" },
        { ModifiedTextRole, "modifiedText" },
        { SuffixRole, "suffix" },
        { IconTextRole, "iconText" },
        { SelectedRole, "selected" },
        { HasReportRole, "hasReport" },
        { HasAlertRole, "hasAlert" },
        { AlertTriggeredRole, "alertTriggered" },
    };
}

QString FileListModel::uriAt(int row) const
{
    if (row < 0 || row >= m_visible.size())
        return {};
    return m_visible.at(row).uri.toString();
}

QString FileListModel::nameAt(int row) const
{
    if (row < 0 || row >= m_visible.size())
        return {};
    return m_visible.at(row).name;
}

int FileListModel::rowOfUri(const QString& uri) const
{
    for (int row = 0; row < m_visible.size(); ++row) {
        if (m_visible.at(row).uri.toString() == uri)
            return row;
    }
    return -1;
}

void FileListModel::setMeasuredSize(const QString& uri, qint64 bytes)
{
    // Recorded even when the row is not visible: a filter can be hiding it, and
    // clearing the filter should not lose a measurement already paid for.
    m_measured.insert(uri, bytes);

    const int row = rowOfUri(uri);
    if (row < 0)
        return;
    const QModelIndex changed = index(row, 0);
    emit dataChanged(changed, changed, { SizeRole, SizeTextRole });
}

qint64 FileListModel::measuredSize(const QString& uri) const
{
    return m_measured.value(uri, -1);
}

QStringList FileListModel::folderUris() const
{
    QStringList out;
    for (const FileEntry& entry : m_visible) {
        if (entry.isDir)
            out.append(entry.uri.toString());
    }
    return out;
}

void FileListModel::pruneSelection()
{
    if (m_selected.isEmpty())
        return;

    QSet<QString> present;
    present.reserve(m_all.size());
    for (const FileEntry& entry : std::as_const(m_all))
        present.insert(entry.uri.toString());

    m_selected.intersect(present);
}

bool FileListModel::isSelected(int row) const
{
    if (row < 0 || row >= m_visible.size())
        return false;
    return m_selected.contains(m_visible.at(row).uri.toString());
}

void FileListModel::setSelected(int row, bool selected)
{
    if (row < 0 || row >= m_visible.size())
        return;

    const QString uri = m_visible.at(row).uri.toString();
    const bool changed = selected ? !m_selected.contains(uri) : m_selected.remove(uri);
    if (selected)
        m_selected.insert(uri);
    if (!changed)
        return;

    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, { SelectedRole });
    emit selectionChanged();
}

void FileListModel::toggleSelected(int row)
{
    setSelected(row, !isSelected(row));
}

void FileListModel::selectAll()
{
    if (m_visible.isEmpty())
        return;
    for (const FileEntry& entry : std::as_const(m_visible))
        m_selected.insert(entry.uri.toString());

    emit dataChanged(index(0, 0), index(static_cast<int>(m_visible.size()) - 1, 0), { SelectedRole });
    emit selectionChanged();
}

void FileListModel::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();

    if (!m_visible.isEmpty())
        emit dataChanged(index(0, 0), index(static_cast<int>(m_visible.size()) - 1, 0), { SelectedRole });
    emit selectionChanged();
}

void FileListModel::invertSelection()
{
    if (m_visible.isEmpty())
        return;
    for (const FileEntry& entry : std::as_const(m_visible)) {
        const QString uri = entry.uri.toString();
        if (!m_selected.remove(uri))
            m_selected.insert(uri);
    }

    emit dataChanged(index(0, 0), index(static_cast<int>(m_visible.size()) - 1, 0), { SelectedRole });
    emit selectionChanged();
}

QStringList FileListModel::selectedUris() const
{
    QStringList out;
    for (const FileEntry& entry : m_visible) {
        const QString uri = entry.uri.toString();
        if (m_selected.contains(uri))
            out.append(uri);
    }
    return out;
}

QList<VfsUri> FileListModel::targets(int fallbackRow) const
{
    QList<VfsUri> out;
    for (const FileEntry& entry : targetEntries(fallbackRow))
        out.append(entry.uri);
    return out;
}

FileEntryList FileListModel::targetEntries(int fallbackRow) const
{
    FileEntryList out;
    for (const FileEntry& entry : m_visible) {
        if (m_selected.contains(entry.uri.toString()))
            out.append(entry);
    }

    // Nothing ticked means "act on whatever the cursor is on", which is what
    // every commander-style manager does.
    if (out.isEmpty() && fallbackRow >= 0 && fallbackRow < m_visible.size())
        out.append(m_visible.at(fallbackRow));

    return out;
}

bool FileListModel::isDirAt(int row) const
{
    if (row < 0 || row >= m_visible.size())
        return false;
    return m_visible.at(row).isDir;
}

qint64 FileListModel::totalSize() const
{
    qint64 total = 0;
    for (const FileEntry& entry : m_visible) {
        if (!entry.isDir)
            total += entry.size;
    }
    return total;
}

} // namespace mole
