#pragma once

#include "core/data/ITableSource.h"
#include "core/tasks/Task.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

class QThread;

namespace mole {

/// Asks a table source one question, on a pool thread.
///
/// The grid used to ask its source from `data()`, which is the thread that draws:
/// for a query with a bounded offset that is cheap enough, and for a Parquet file
/// written as one row group it was the whole file read and decompressed to show
/// fifty rows of it. See MOLE-287 and ADR-0079.
///
/// **One question at a time, and the source says whether it may be asked here at
/// all.** A source is not promised to be thread-safe -- see
/// ITableSource::canBeReadOnATask() -- so the model keeps one of these
/// outstanding and queues the rest.
///
/// The source is shared rather than borrowed. A reader who steps off a file while
/// a read is running is the ordinary case, not an unusual one, and a task holding
/// a raw pointer to a source the viewer has just deleted is the fault MOLE-290
/// spent a day on in the importer.
class ReadTableTask final : public Task
{
    Q_OBJECT

public:
    enum class Question {
        Window, ///< rows(offset, limit, filter)
        MatchCount, ///< matchingRows(filter)
        ColumnWidths, ///< columnWidths(limit)
    };

    ReadTableTask(std::shared_ptr<const ITableSource> source, Question question, qint64 offset, int limit,
        QString filter, QObject* parent = nullptr);

    Question question() const { return m_question; }
    qint64 offset() const { return m_offset; }
    int limit() const { return m_limit; }
    QString filter() const { return m_filter; }

    // ---- the answer, valid once finished() and only for the question asked ----

    const QList<QStringList>& rows() const { return m_rows; }
    /// False when the window could not be read at all, which is not a window that
    /// held nothing -- see ITableSource::rows().
    bool wasReadable() const { return m_readable; }
    qint64 count() const { return m_count; }
    const QList<int>& widths() const { return m_widths; }
    /// The thread run() executed on, so a test can hold the house rule that the
    /// thread which draws does not read files.
    QThread* ranOn() const { return m_ranOn; }

protected:
    void run() override;

private:
    std::shared_ptr<const ITableSource> m_source;
    Question m_question = Question::Window;
    qint64 m_offset = 0;
    int m_limit = 0;
    QString m_filter;

    QList<QStringList> m_rows;
    bool m_readable = true;
    qint64 m_count = -1;
    QList<int> m_widths;
    QThread* m_ranOn = nullptr;
};

} // namespace mole
