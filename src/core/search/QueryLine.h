#pragma once

#include <QList>
#include <QString>

namespace mole {

/// One thing somebody typed on the query line.
///
/// `report ext:pdf size>10M -path:node_modules` is four of these: a bare word,
/// a key and a value, a key with a comparison, and a negated one.
struct QueryTerm
{
    /// Empty for a bare word, which is a name substring -- what a bare word
    /// means in every search box anybody has used.
    QString key;
    QString value;

    enum class Op {
        Is, ///< key:value
        AtLeast, ///< key>=value
        AtMost, ///< key<=value
        Above, ///< key>value
        Below, ///< key<value
    };
    Op op = Op::Is;

    /// `-key:value`. The one negation there is, for the same reason the form
    /// has only two: a general boolean language is a different feature.
    bool negate = false;

    /// Where the value was written, so a complaint about it can point at it.
    int position = 0;
    int length = 0;

    /// True when the value was written between slashes, which is the one place
    /// a pattern is guessed at rather than chosen from a control.
    bool isRegex = false;
    /// True when it was quoted, so printing it again quotes it again.
    bool wasQuoted = false;
};

/// What is wrong with a line, and where.
///
/// A query that cannot be read does not run. `size>10Q` matching everything
/// would be how somebody spends ten minutes doubting their disk.
struct QueryLineError
{
    QString message;
    int position = 0;
    int length = 0;
};

struct ParsedQueryLine
{
    QList<QueryTerm> terms;
    QList<QueryLineError> errors;

    bool ok() const { return errors.isEmpty(); }
};

/// Reads a query line into terms.
///
/// Syntax only: this knows about words, keys, operators, quotes, slashes and
/// commas, and nothing at all about which keys exist or what their values may
/// be. That vocabulary belongs to whoever holds the criteria, because it grows
/// with the index -- a camera is a key on a volume that recorded one and not on
/// a volume that did not.
///
/// **Everything is `and`.** There is no `or`, no parentheses and no precedence,
/// and that is a decision rather than an omission: a general boolean language is
/// a different feature with a different interface, and it must not arrive here
/// one operator at a time.
[[nodiscard]] ParsedQueryLine parseQueryLine(const QString& text);

/// Writes terms back out, quoting whatever needs it.
///
/// Printing what was parsed has to give the line back: the field and the line
/// are one query seen twice, and a round trip that drifts would have them
/// fighting each other while somebody types.
[[nodiscard]] QString printQueryLine(const QList<QueryTerm>& terms);

} // namespace mole
