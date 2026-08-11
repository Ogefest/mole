#pragma once

#include "core/vfs/FileEntry.h"

#include <QAbstractListModel>
#include <QDateTime>
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
        /// Whether this row is what is on disk now or what a scan recorded.
        ProvenanceRole,
        /// When that scan ran. Invalid for a row seen now.
        IndexedAtRole,
    };

    /// Where a row came from.
    ///
    /// A list that mixes what is on disk now with what a scan recorded is only
    /// an answer anybody can reason about if the row says which it is. The
    /// marking is not a decoration on the feature -- it is the feature.
    enum Provenance {
        SeenNow = 0, ///< a listing or a walk found it, just now
        FromIndex, ///< a previous scan recorded it, and nothing has checked since
    };
    Q_ENUM(Provenance)

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
    /// The same, except that a row already here for the same uri is replaced
    /// where it sits rather than added again.
    ///
    /// What a search does when it primed its list from an index and is now
    /// walking the same tree: the walk's answer supersedes the scan's, in
    /// place, so the row stops being marked as remembered and starts being
    /// what is there.
    void mergeEntries(const FileEntryList& entries);
    /// Withdraws rows by uri. What a search does when the walk reaches a
    /// directory and finds that something the index claimed is not there.
    void removeEntries(const QStringList& uris);

    /// Marks rows as recorded by a scan that ran at `scannedAt`. Anything not
    /// marked is what it always was: seen now.
    void markFromIndex(const QStringList& uris, const QDateTime& scannedAt);
    /// How many rows are still only what a scan remembered.
    Q_INVOKABLE int fromIndexCount() const { return static_cast<int>(m_indexed.size()); }

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
    int totalCount() const { return static_cast<int>(m_all.size() - m_withdrawn.size()); }
    SortKey sortKey() const { return m_sortKey; }
    void setSortKey(SortKey key);
    bool sortDescending() const { return m_sortDescending; }
    void setSortDescending(bool descending);

    // ---- measured folder sizes -------------------------------------------
    //
    // A directory's own `size` is the inode's, not what is inside it, so a
    // measured total is kept separately rather than written over the entry --
    // one field that is sometimes one thing and sometimes another is a field
    // nobody can trust. Keyed by uri, so sorting and refiltering cannot move a
    // number onto the wrong row.

    /// Fills in what a folder was measured to contain. Ignores a uri that is not
    /// in this listing, which happens when the answer arrives after the user has
    /// moved on.
    Q_INVOKABLE void setMeasuredSize(const QString& uri, qint64 bytes);
    /// -1 when this folder has not been measured.
    Q_INVOKABLE qint64 measuredSize(const QString& uri) const;
    /// Every folder in the listing, for "measure these" when nothing is picked.
    Q_INVOKABLE QStringList folderUris() const;

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
    /// The same rows, whole. What an operation is aimed at gets *shown* before it
    /// runs, and a list of bare uris cannot say whether something is a folder or
    /// how big it is -- so both come from here rather than the caller looking the
    /// rows up again and risking a different answer.
    FileEntryList targetEntries(int fallbackRow) const;
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
    /// What a row is showing. Rows are offsets into m_all, so every reader goes
    /// through here rather than remembering which list it is holding.
    const FileEntry& entryAt(int row) const { return m_all.at(m_visible.at(row)); }
    int annotationFor(const QString& uri) const { return m_annotations.value(uri, NoAnnotation); }
    bool lessThan(const FileEntry& a, const FileEntry& b) const;

    QHash<QString, int> m_annotations;

    struct IndexedRow
    {
        int offset = -1; ///< into m_all
        QDateTime scannedAt;
    };
    /// The rows a scan supplied, by uri. Only these can be superseded or
    /// withdrawn, so only these need an index into m_all -- keeping the map to
    /// the indexed half rather than to every row a long search finds.
    QHash<QString, IndexedRow> m_indexed;

    /// Drops selected uris that are no longer present.
    void pruneSelection();

    FileEntryList m_all;
    /// The rows, as offsets into m_all rather than copies of what is in it.
    ///
    /// Copies were simpler, and were what this held, until streaming search
    /// results had to arrive as insertions rather than a reset. An insertion
    /// into the middle of a list shifts everything after it, and a batch of two
    /// hundred results scattered through forty thousand does that two hundred
    /// times -- so what gets shifted matters. An offset is four bytes moved by
    /// memmove; a FileEntry is a name, a URI and a date moved one at a time.
    /// Measured over the same forty thousand results in two hundred batches:
    /// 29 seconds holding copies, a fraction of a second holding offsets.
    ///
    /// Safe because m_all only ever grows or is replaced wholesale -- nothing
    /// removes a single entry from it, which is what would strand an offset.
    QList<int> m_visible;
    /// Offsets in m_all that have been taken back.
    ///
    /// Withdrawn rather than erased: m_visible holds offsets into m_all, so
    /// erasing one entry would strand every offset after it. Nothing else in
    /// this class removes a single entry, and this is why.
    QSet<int> m_withdrawn;
    QSet<QString> m_selected;
    /// Dropped whenever the listing is replaced: a measurement belongs to the
    /// tree as it was when it was taken.
    QHash<QString, qint64> m_measured;
    bool m_showHidden = false;
    QString m_filterText;
    /// Unicode-lowercased once, so filtering does not re-fold on every row.
    QString m_filterFolded;
    SortKey m_sortKey = SortKey::Name;
    bool m_sortDescending = false;
};

} // namespace mole
