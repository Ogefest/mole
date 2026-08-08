#pragma once

#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>

namespace mole {

/// A parsed CSV/TSV file.
struct DelimitedTable
{
    QStringList headers;
    QList<QStringList> rows;
    /// What was actually used, whether given or detected.
    QChar separator;
    /// True when parsing stopped at the row limit rather than the end.
    bool truncated = false;

    int columnCount() const;
    bool isEmpty() const { return headers.isEmpty() && rows.isEmpty(); }
};

/// Reads delimited text the way spreadsheets write it.
///
/// Follows RFC 4180 where it matters: quoted fields, `""` for a literal quote,
/// separators and newlines inside quotes. Real files break the rules often
/// enough that nothing here is fatal -- a ragged row is padded, an unterminated
/// quote runs to the end, and parsing always produces something.
class DelimitedTextParser
{
public:
    struct Options
    {
        /// Null means detect from the content.
        QChar separator = QChar();
        QChar quote = QLatin1Char('"');
        bool firstRowIsHeader = true;
        /// Previewing a million-row export must not eat the memory.
        int maxRows = 5000;
    };

    /// Picks the separator that splits the sample most consistently.
    /// Falls back to a comma when nothing looks convincing.
    static QChar detectSeparator(const QString& sample);

    // Two overloads rather than a defaulted argument: Options carries default
    // member initializers, which are not usable in a default argument of the
    // class that encloses them.
    static DelimitedTable parse(const QString& text);
    static DelimitedTable parse(const QString& text, const Options& options);

    /// Candidates in the order they are tried, for the UI to offer.
    static QList<QChar> candidateSeparators();
};

} // namespace mole
