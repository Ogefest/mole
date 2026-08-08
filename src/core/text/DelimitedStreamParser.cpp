#include "core/text/DelimitedStreamParser.h"

namespace mole {

DelimitedStreamParser::DelimitedStreamParser(QChar separator, QChar quote)
    : m_separator(separator)
    , m_quote(quote)
{
}

void DelimitedStreamParser::reset()
{
    m_row.clear();
    m_field.clear();
    m_inQuotes = false;
    m_fieldStarted = false;
    m_pendingQuote = false;
    m_lastWasCarriageReturn = false;
}

QList<QStringList> DelimitedStreamParser::feed(const QString& text)
{
    QList<QStringList> completed;

    for (const QChar c : text) {
        // A quote seen while inside a quoted field is ambiguous until the next
        // character arrives: `""` is a literal quote, `",` closes the field.
        if (m_pendingQuote) {
            m_pendingQuote = false;
            if (c == m_quote) {
                m_field.append(m_quote);
                continue;
            }
            m_inQuotes = false;
            // fall through and handle `c` as ordinary text
        }

        if (m_inQuotes) {
            if (c == m_quote) {
                m_pendingQuote = true;
                continue;
            }
            m_field.append(c);
            continue;
        }

        if (c == m_quote && !m_fieldStarted) {
            m_inQuotes = true;
            m_fieldStarted = true;
            continue;
        }

        if (c == m_separator) {
            m_row.append(m_field);
            m_field.clear();
            m_fieldStarted = false;
            m_lastWasCarriageReturn = false;
            continue;
        }

        if (c == QLatin1Char('\r')) {
            m_row.append(m_field);
            m_field.clear();
            m_fieldStarted = false;
            completed.append(m_row);
            m_row.clear();
            m_lastWasCarriageReturn = true;
            continue;
        }

        if (c == QLatin1Char('\n')) {
            if (m_lastWasCarriageReturn) {
                // The break was already taken when the \r arrived.
                m_lastWasCarriageReturn = false;
                continue;
            }
            m_row.append(m_field);
            m_field.clear();
            m_fieldStarted = false;
            completed.append(m_row);
            m_row.clear();
            continue;
        }

        m_lastWasCarriageReturn = false;
        m_field.append(c);
        m_fieldStarted = true;
    }

    return completed;
}

QStringList DelimitedStreamParser::finish()
{
    if (m_pendingQuote) {
        m_pendingQuote = false;
        m_inQuotes = false;
    }

    // A file that ends without a line break still has a last row; dropping it
    // would quietly lose a line from every such export.
    if (m_field.isEmpty() && m_row.isEmpty())
        return {};

    m_row.append(m_field);
    QStringList last = m_row;
    m_row.clear();
    m_field.clear();
    m_fieldStarted = false;
    return last;
}

} // namespace mole
