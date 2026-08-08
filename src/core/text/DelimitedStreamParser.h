#pragma once

#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>

namespace mole {

/// Parses delimited text a piece at a time.
///
/// The whole-file parser cannot help with a 100 GB export: the file never fits
/// in memory, so it has to be read in chunks. That means a row can straddle a
/// chunk boundary, and so can a quoted field containing newlines. This keeps
/// exactly that much state and nothing else.
///
/// Follows RFC 4180 where it matters -- quoted fields, `""` for a literal
/// quote, separators and newlines inside quotes -- and, like the whole-file
/// parser, treats every real-world violation as something to survive rather
/// than something to reject.
class DelimitedStreamParser
{
public:
    explicit DelimitedStreamParser(QChar separator = QLatin1Char(','), QChar quote = QLatin1Char('"'));

    void setSeparator(QChar separator) { m_separator = separator; }
    void setQuote(QChar quote) { m_quote = quote; }

    /// Feeds more text and returns whatever rows became complete. A row still
    /// being built is kept for the next call.
    QList<QStringList> feed(const QString& text);

    /// Returns a final row if the file ended without a line break. Must be
    /// called once at the end, or the last line of a file is silently lost.
    QStringList finish();

    /// Resets everything, for re-reading with a different separator.
    void reset();

    /// True while the parser is inside a quoted field, i.e. a newline seen now
    /// would be data rather than a row break.
    bool isInsideQuotes() const { return m_inQuotes; }

private:
    QChar m_separator;
    QChar m_quote;

    QStringList m_row;
    QString m_field;
    bool m_inQuotes = false;
    bool m_fieldStarted = false;
    /// A quote inside a quoted field: the next character decides whether it
    /// closed the field or was an escaped quote.
    bool m_pendingQuote = false;
    /// So "\r\n" is one break and not two.
    bool m_lastWasCarriageReturn = false;
};

} // namespace mole
