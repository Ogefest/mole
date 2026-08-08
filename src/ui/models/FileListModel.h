#pragma once

#include "core/vfs/FileEntry.h"

#include <QAbstractListModel>
#include <QHash>
#include <QSet>

namespace mole {

/// Presents a list of FileEntry to QML. Deliberately passive: it does no I/O
/// and knows nothing about navigation, which keeps it trivial to test and
/// lets browsing, live search and index search all reuse it.
class FileListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)
    Q_PROPERTY(SortKey sortKey READ sortKey WRITE setSortKey NOTIFY sortChanged)
    Q_PROPERTY(bool sortDescending READ sortDescending WRITE setSortDescending NOTIFY sortChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        UriRole,
        ParentUriRole,
        IsDirRole,
        IsHiddenRole,
        SizeRole,
        SizeTextRole,
        ModifiedRole,
        ModifiedTextRole,
        SuffixRole,
        IconTextRole,
        SelectedRole,
        /// A saved report exists for this folder.
        HasReportRole,
        /// An alert is watching this entry, and whether it has tripped.
        HasAlertRole,
        AlertTriggeredRole,
    };

    enum class SortKey { Name, Size, Modified, Type };
    Q_ENUM(SortKey)

    /// What is known about an entry beyond what the drive reported.
    enum Annotation {
        NoAnnotation = 0,
        ReportPresent = 1 << 0,
        AlertPresent = 1 << 1,
        AlertTriggered = 1 << 2,
    };

    explicit FileListModel(QObject* parent = nullptr);

    /// Marks entries with what the application knows about them, keyed by uri.
    /// Computed by the pane controller, which is the only layer with both the
    /// listing and the stores; the model just carries it to the delegate.
    void setAnnotations(QHash<QString, int> annotations);

    /// Replaces the contents.
    void setEntries(FileEntryList entries);
    /// Appends without disturbing what is already shown -- used by searches
    /// that stream their results in.
    void appendEntries(const FileEntryList& entries);
    Q_INVOKABLE void clear();

    /// Directories first, then by the current sort key.
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);

    /// Narrows the listing to names containing this text. Unlike a search it
    /// never leaves the current directory and never touches storage -- it only
    /// hides rows that are already loaded.
    QString filterText() const { return m_filterText; }
    void setFilterText(const QString& text);
    /// Rows before filtering, for "12 of 340" in the status line.
    int totalCount() const { return static_cast<int>(m_all.size()); }
    SortKey sortKey() const { return m_sortKey; }
    void setSortKey(SortKey key);
    bool sortDescending() const { return m_sortDescending; }
    void setSortDescending(bool descending);

    Q_INVOKABLE QString uriAt(int row) const;
    Q_INVOKABLE QString nameAt(int row) const;
    Q_INVOKABLE bool isDirAt(int row) const;
    Q_INVOKABLE int rowOfUri(const QString& uri) const;

    // ---- selection -------------------------------------------------------
    //
    // Tracked by uri rather than by row, so re-sorting or refreshing a
    // directory does not silently move the selection onto other files.

    int selectionCount() const { return static_cast<int>(m_selected.size()); }
    Q_INVOKABLE bool isSelected(int row) const;
    Q_INVOKABLE void setSelected(int row, bool selected);
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void invertSelection();

    /// The selection, or the single row at `fallbackRow` when nothing is
    /// selected -- the way a commander-style pane behaves.
    QList<VfsUri> targets(int fallbackRow) const;
    Q_INVOKABLE QStringList selectedUris() const;
    /// Total bytes of the visible files, for the status line.
    Q_INVOKABLE qint64 totalSize() const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Shared by the models and the status line so sizes read the same
    /// everywhere in the app.
    static QString formatSize(qint64 bytes);

signals:
    void countChanged();
    void showHiddenChanged();
    void sortChanged();
    void selectionChanged();
    void filterChanged();

private:
    void rebuildVisible();
    int annotationFor(const QString& uri) const { return m_annotations.value(uri, NoAnnotation); }
    bool lessThan(const FileEntry& a, const FileEntry& b) const;

    QHash<QString, int> m_annotations;

    /// Drops selected uris that are no longer present.
    void pruneSelection();

    FileEntryList m_all;
    FileEntryList m_visible;
    QSet<QString> m_selected;
    bool m_showHidden = false;
    QString m_filterText;
    /// Unicode-lowercased once, so filtering does not re-fold on every row.
    QString m_filterFolded;
    SortKey m_sortKey = SortKey::Name;
    bool m_sortDescending = false;
};

} // namespace mole
