#include "core/search/QueryLine.h"

#include <QStringList>

namespace mole {
namespace {

    bool isKeyChar(QChar ch)
    {
        // A key is a word with dots in it, because the metadata keys are
        // namespaced -- `image.camera` is one key and not a word, a dot and
        // another word.
        return ch.isLetterOrNumber() || ch == QLatin1Char('.') || ch == QLatin1Char('_');
    }

    /// Reads one word, honouring quotes and slashes. Returns false when a quote
    /// or a slash is never closed, which is the one syntax error there is.
    bool readValue(const QString& text, int& at, QString& out, bool& quoted, bool& regex, int& length)
    {
        const int start = at;
        quoted = false;
        regex = false;

        if (at < text.size() && (text.at(at) == QLatin1Char('"') || text.at(at) == QLatin1Char('\''))) {
            const QChar quote = text.at(at++);
            while (at < text.size() && text.at(at) != quote)
                out += text.at(at++);
            if (at >= text.size())
                return false;
            ++at; // the closing quote
            quoted = true;
            length = at - start;
            return true;
        }

        if (at < text.size() && text.at(at) == QLatin1Char('/')) {
            ++at;
            while (at < text.size() && text.at(at) != QLatin1Char('/')) {
                if (text.at(at) == QLatin1Char('\\') && at + 1 < text.size())
                    out += text.at(at++);
                out += text.at(at++);
            }
            if (at >= text.size())
                return false;
            ++at; // the closing slash
            regex = true;
            length = at - start;
            return true;
        }

        while (at < text.size() && !text.at(at).isSpace())
            out += text.at(at++);
        length = at - start;
        return true;
    }

} // namespace

ParsedQueryLine parseQueryLine(const QString& text)
{
    ParsedQueryLine parsed;

    int at = 0;
    while (at < text.size()) {
        while (at < text.size() && text.at(at).isSpace())
            ++at;
        if (at >= text.size())
            break;

        const int termStart = at;
        QueryTerm term;

        if (text.at(at) == QLatin1Char('-') && at + 1 < text.size() && !text.at(at + 1).isSpace()) {
            term.negate = true;
            ++at;
        }

        // A key, if this looks like one. A bare word containing a colon whose
        // left half is not a key is still a bare word -- the caller decides
        // that, so a file really called `notes:2026.txt` is findable.
        const int keyStart = at;
        int keyEnd = at;
        while (keyEnd < text.size() && isKeyChar(text.at(keyEnd)))
            ++keyEnd;

        bool hasKey = false;
        if (keyEnd > keyStart && keyEnd < text.size()) {
            const QChar next = text.at(keyEnd);
            if (next == QLatin1Char(':')) {
                term.op = QueryTerm::Op::Is;
                hasKey = true;
                at = keyEnd + 1;
            } else if (next == QLatin1Char('>') || next == QLatin1Char('<')) {
                const bool orEqual = keyEnd + 1 < text.size() && text.at(keyEnd + 1) == QLatin1Char('=');
                term.op = next == QLatin1Char('>') ? (orEqual ? QueryTerm::Op::AtLeast : QueryTerm::Op::Above)
                                                   : (orEqual ? QueryTerm::Op::AtMost : QueryTerm::Op::Below);
                hasKey = true;
                at = keyEnd + (orEqual ? 2 : 1);
            }
        }
        if (hasKey)
            term.key = text.mid(keyStart, keyEnd - keyStart);
        else
            at = keyStart;

        // A comparison after a colon, which is how a date range reads best:
        // `modified:<30d` rather than `modified<30d`.
        if (hasKey && at < text.size()
            && (text.at(at) == QLatin1Char('>') || text.at(at) == QLatin1Char('<'))) {
            const QChar sign = text.at(at++);
            const bool orEqual = at < text.size() && text.at(at) == QLatin1Char('=');
            if (orEqual)
                ++at;
            term.op = sign == QLatin1Char('>') ? (orEqual ? QueryTerm::Op::AtLeast : QueryTerm::Op::Above)
                                               : (orEqual ? QueryTerm::Op::AtMost : QueryTerm::Op::Below);
        }

        int valueLength = 0;
        if (!readValue(text, at, term.value, term.wasQuoted, term.isRegex, valueLength)) {
            parsed.errors.append({ QStringLiteral("This is never closed"), termStart,
                static_cast<int>(text.size()) - termStart });
            return parsed;
        }

        term.position = at - valueLength;
        term.length = valueLength;

        // `-` on its own, or a key with nothing after it.
        if (term.value.isEmpty() && !term.wasQuoted) {
            parsed.errors.append({ hasKey ? QStringLiteral("%1 was given nothing to match").arg(term.key)
                                          : QStringLiteral("A dash on its own matches nothing"),
                termStart, qMax(1, at - termStart) });
            return parsed;
        }

        parsed.terms.append(term);
    }

    return parsed;
}

QString printQueryLine(const QList<QueryTerm>& terms)
{
    QStringList out;
    for (const QueryTerm& term : terms) {
        QString one;
        if (term.negate)
            one += QLatin1Char('-');
        if (!term.key.isEmpty()) {
            one += term.key;
            switch (term.op) {
            case QueryTerm::Op::Is:
                one += QLatin1Char(':');
                break;
            case QueryTerm::Op::AtLeast:
                one += QStringLiteral(">=");
                break;
            case QueryTerm::Op::AtMost:
                one += QStringLiteral("<=");
                break;
            case QueryTerm::Op::Above:
                one += QLatin1Char('>');
                break;
            case QueryTerm::Op::Below:
                one += QLatin1Char('<');
                break;
            }
        }

        if (term.isRegex)
            one += QLatin1Char('/') + term.value + QLatin1Char('/');
        else if (term.wasQuoted || term.value.contains(QLatin1Char(' ')) || term.value.isEmpty())
            one += QLatin1Char('"') + term.value + QLatin1Char('"');
        else
            one += term.value;

        out.append(one);
    }
    return out.join(QLatin1Char(' '));
}

} // namespace mole
