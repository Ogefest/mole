#include "core/data/ReadTableTask.h"

#include <QThread>

namespace mole {
namespace {

    QString titleFor(ReadTableTask::Question question)
    {
        switch (question) {
        case ReadTableTask::Question::MatchCount:
            return QStringLiteral("Counting matching rows");
        case ReadTableTask::Question::ColumnWidths:
            return QStringLiteral("Measuring columns");
        case ReadTableTask::Question::Window:
            break;
        }
        return QStringLiteral("Reading rows");
    }

} // namespace

ReadTableTask::ReadTableTask(std::shared_ptr<const ITableSource> source, Question question, qint64 offset,
    int limit, QString filter, QObject* parent)
    : Task(titleFor(question), parent)
    , m_source(std::move(source))
    , m_question(question)
    , m_offset(offset)
    , m_limit(limit)
    , m_filter(std::move(filter))
{
    // One of a crowd: scrolling a table is one of these per chunk and none of them
    // is a job anybody remembers starting. Not background, though -- looking at the
    // rows is exactly asking for them. See ADR-0064.
    setOneOfMany(true);
}

void ReadTableTask::run()
{
    m_ranOn = QThread::currentThread();
    if (!m_source || isCancelRequested())
        return;

    // Checked before the read and not during it: ITableSource takes no cancel
    // token, so a read that has begun runs to its end and cancelling means the
    // answer is thrown away when it lands. What bounds that wait is the bound on
    // the read itself -- a batch and a page for the one source that asks to be
    // read here, which is what MOLE-287 put there.
    switch (m_question) {
    case Question::Window:
        m_rows = m_source->rows(m_offset, m_limit, m_filter, &m_readable);
        break;
    case Question::MatchCount:
        m_count = m_source->matchingRows(m_filter);
        break;
    case Question::ColumnWidths:
        m_widths = m_source->columnWidths(m_limit);
        break;
    }
}

} // namespace mole
