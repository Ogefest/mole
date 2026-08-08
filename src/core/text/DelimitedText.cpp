#include "core/text/DelimitedText.h"

#include <QHash>

#include <algorithm>

namespace mole {
namespace {

    constexpr int kDetectionSampleLines = 20;

    /// Splits one logical record, honouring quotes. Returns the offset just
    /// past the record so the caller can continue -- a quoted field may
    /// contain newlines, so records and lines are not the same thing.
    int parseRecord(const QString& text, int start, QChar separator, QChar quote, QStringList& out)
    {
        QString field;
        bool inQuotes = false;
        int i = start;

        for (; i < text.size(); ++i) {
            const QChar c = text.at(i);

            if (inQuotes) {
                if (c != quote) {
                    field.append(c);
                    continue;
                }
                // A doubled quote inside a quoted field is a literal quote.
                if (i + 1 < text.size() && text.at(i + 1) == quote) {
                    field.append(quote);
                    ++i;
                    continue;
                }
                inQuotes = false;
                continue;
            }

            if (c == quote && field.isEmpty()) {
                inQuotes = true;
                continue;
            }
            if (c == separator) {
                out.append(field);
                field.clear();
                continue;
            }
            if (c == QLatin1Char('\n')) {
                ++i;
                break;
            }
            if (c == QLatin1Char('\r')) {
                // Swallow CRLF as one break.
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                    ++i;
                ++i;
                break;
            }
            field.append(c);
        }

        out.append(field);
        return i;
    }

} // namespace

int DelimitedTable::columnCount() const
{
    int columns = headers.size();
    for (const QStringList& row : rows)
        columns = std::max(columns, static_cast<int>(row.size()));
    return columns;
}

QList<QChar> DelimitedTextParser::candidateSeparators()
{
    return { QLatin1Char(','), QLatin1Char('\t'), QLatin1Char(';'), QLatin1Char('|') };
}

QChar DelimitedTextParser::detectSeparator(const QString& sample)
{
    // The right separator is the one that gives every line the same number of
    // fields, and more than one. Counting occurrences alone picks whatever
    // punctuation happens to be common inside the data.
    QChar best = QLatin1Char(',');
    int bestScore = -1;

    for (const QChar candidate : candidateSeparators()) {
        QList<int> counts;
        int offset = 0;
        while (offset < sample.size() && counts.size() < kDetectionSampleLines) {
            QStringList fields;
            offset = parseRecord(sample, offset, candidate, QLatin1Char('"'), fields);
            if (fields.size() == 1 && fields.first().isEmpty())
                continue;
            counts.append(static_cast<int>(fields.size()));
        }

        if (counts.isEmpty())
            continue;

        const int columns = counts.first();
        if (columns < 2)
            continue;

        const bool consistent
            = std::all_of(counts.begin(), counts.end(), [columns](int n) { return n == columns; });

        // Consistency first, then the separator that yields more columns --
        // a file split on both ';' and ',' is usually the former.
        const int score = (consistent ? 1000 : 0) + columns;
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }

    return best;
}

DelimitedTable DelimitedTextParser::parse(const QString& text)
{
    return parse(text, Options());
}

DelimitedTable DelimitedTextParser::parse(const QString& text, const Options& options)
{
    DelimitedTable table;
    if (text.isEmpty())
        return table;

    table.separator = options.separator.isNull() ? detectSeparator(text) : options.separator;

    int offset = 0;
    bool first = true;
    while (offset < text.size()) {
        if (table.rows.size() >= options.maxRows) {
            table.truncated = true;
            break;
        }

        QStringList fields;
        offset = parseRecord(text, offset, table.separator, options.quote, fields);

        // A trailing newline yields one empty field; that is not a row.
        if (fields.size() == 1 && fields.first().isEmpty() && offset >= text.size())
            break;

        if (first && options.firstRowIsHeader) {
            table.headers = fields;
            first = false;
            continue;
        }
        first = false;
        table.rows.append(fields);
    }

    // Ragged rows are common in exports. Pad rather than drop, so a single
    // malformed line does not hide the rest of the file.
    const int columns = table.columnCount();
    for (QStringList& row : table.rows) {
        while (row.size() < columns)
            row.append(QString());
    }
    while (!table.headers.isEmpty() && table.headers.size() < columns)
        table.headers.append(QString());

    return table;
}

} // namespace mole
