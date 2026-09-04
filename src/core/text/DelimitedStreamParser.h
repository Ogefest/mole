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

    /// The most one field may hold before the quote around it is disbelieved.
    ///
    /// **The whole-file parser's rule is "an unterminated quote runs to the
    /// end", and there it is bounded by the viewer's 512 kB window. Here there
    /// was nothing.** One `"` at the start of a field with no closing quote --
    /// a hand-edited export, a field like `"5 inch, 3 pcs` -- turned everything
    /// after it in a 40 GB file into a single field, and the field grew to twice
    /// the remaining file in UTF-16 until the process was killed. The design
    /// TODO.md describes, "1 MB chunks, 5,000-row batches, nothing held whole",
    /// was defeated by one byte, and it looked like a hang. See MOLE-367.
    ///
    /// 16 MB rather than something small: a legitimate quoted field can be a
    /// pasted document, and cutting one of those short would be a wrong answer
    /// where this is a refusal of something that cannot be right.
    static constexpr int kMaxFieldChars = 16 * 1024 * 1024;

    /// How many rows had a quoted field that ran past kMaxFieldChars.
    ///
    /// Reported rather than counted silently: the rows after such a quote are
    /// split on separators the file's author never meant as separators, so the
    /// caller has something to say about why the grid looks odd.
    int malformedRows() const { return m_malformedRows; }

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
    /// Set when the field being built has already overrun, so the overrun is
    /// counted once per row rather than once per character.
    bool m_fieldOverran = false;
    int m_malformedRows = 0;
};

} // namespace mole
