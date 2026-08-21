#include "core/rename/RenamePlan.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace mole {
namespace {

    /// Splits a name into the part rules usually touch and the part they usually
    /// must not. A leading dot is part of the stem: ".gitignore" has no extension,
    /// and treating "gitignore" as one would rename the file to nothing.
    void splitName(const QString& name, QString* stem, QString* extension)
    {
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0) {
            *stem = name;
            extension->clear();
            return;
        }
        *stem = name.left(dot);
        *extension = name.mid(dot + 1);
    }

    QString joinName(const QString& stem, const QString& extension)
    {
        return extension.isEmpty() ? stem : stem + QLatin1Char('.') + extension;
    }

    /// Where a removal starts. Negative counts characters back from the end, so -3
    /// with a length of 3 takes the last three.
    int resolveIndex(int position, int size)
    {
        return std::clamp(position < 0 ? size + position : position, 0, size);
    }

    /// Where an insertion goes. Insertion points sit *between* characters, so there
    /// is one more of them than there are characters and -1 means "at the end" --
    /// a different convention from a removal's, because each is the natural one for
    /// its operation.
    int resolveInsertPoint(int position, int size)
    {
        return std::clamp(position < 0 ? size + position + 1 : position, 0, size);
    }

    QString titleCase(const QString& text)
    {
        QString out = text;
        bool atStart = true;
        for (int i = 0; i < out.size(); ++i) {
            if (out.at(i).isLetter()) {
                out[i] = atStart ? out.at(i).toUpper() : out.at(i).toLower();
                atStart = false;
            } else {
                atStart = true;
            }
        }
        return out;
    }

    QString sentenceCase(const QString& text)
    {
        QString out = text.toLower();
        for (int i = 0; i < out.size(); ++i) {
            if (out.at(i).isLetter()) {
                out[i] = out.at(i).toUpper();
                break;
            }
        }
        return out;
    }

    QString stripAccents(const QString& text)
    {
        // Decompose, then drop the combining marks. This turns "Kraków" into
        // "Krakow" rather than into "Krakw", which is what removing non-ASCII
        // wholesale would do.
        const QString decomposed = text.normalized(QString::NormalizationForm_D);
        QString out;
        out.reserve(decomposed.size());
        for (const QChar c : decomposed) {
            if (c.category() != QChar::Mark_NonSpacing)
                out.append(c);
        }
        return out;
    }

    QString applyToPart(const QString& part, const RenameRule& rule, int index)
    {
        QString out = part;

        switch (rule.kind) {
        case RenameRule::Kind::Replace: {
            if (rule.find.isEmpty())
                break;
            if (rule.useRegex) {
                QRegularExpression pattern(rule.find,
                    rule.caseSensitive ? QRegularExpression::NoPatternOption
                                       : QRegularExpression::CaseInsensitiveOption);
                // An invalid pattern leaves the name alone. Half-applying a broken
                // expression would be worse than doing nothing.
                if (pattern.isValid())
                    out.replace(pattern, rule.replaceWith);
            } else {
                out.replace(rule.find, rule.replaceWith,
                    rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
            }
            break;
        }
        case RenameRule::Kind::Case:
            switch (rule.caseStyle) {
            case RenameRule::CaseStyle::Upper:
                out = out.toUpper();
                break;
            case RenameRule::CaseStyle::Lower:
                out = out.toLower();
                break;
            case RenameRule::CaseStyle::Title:
                out = titleCase(out);
                break;
            case RenameRule::CaseStyle::Sentence:
                out = sentenceCase(out);
                break;
            }
            break;
        case RenameRule::Kind::Insert:
            out.insert(resolveInsertPoint(rule.position, out.size()), rule.text);
            break;
        case RenameRule::Kind::Remove: {
            if (rule.length <= 0)
                break;
            const int from = resolveIndex(rule.position, out.size());
            out.remove(from, rule.length);
            break;
        }
        case RenameRule::Kind::Strip: {
            QString kept;
            kept.reserve(out.size());
            if (rule.stripClass == RenameRule::StripClass::Accents) {
                out = stripAccents(out);
                break;
            }
            for (const QChar c : std::as_const(out)) {
                bool drop = false;
                switch (rule.stripClass) {
                case RenameRule::StripClass::Digits:
                    drop = c.isDigit();
                    break;
                case RenameRule::StripClass::Punctuation:
                    drop = c.isPunct() || c.isSymbol();
                    break;
                case RenameRule::StripClass::Whitespace:
                    drop = c.isSpace();
                    break;
                case RenameRule::StripClass::NonAscii:
                    drop = c.unicode() > 127;
                    break;
                case RenameRule::StripClass::Accents:
                    break;
                }
                if (!drop)
                    kept.append(c);
            }
            out = kept;
            break;
        }
        case RenameRule::Kind::Number: {
            const int value = rule.start + index * rule.step;
            const QString counter
                = QStringLiteral("%1").arg(value, std::max(1, rule.padding), 10, QLatin1Char('0'));
            const QString piece
                = rule.numberAt < 0 ? rule.numberSeparator + counter : counter + rule.numberSeparator;
            out.insert(resolveInsertPoint(rule.numberAt, out.size()), piece);
            break;
        }
        case RenameRule::Kind::Affix:
            out = rule.prefix + out + rule.suffix;
            break;
        case RenameRule::Kind::Extension:
            // Handled at the whole-name level; the extension is not a "part" a
            // stem-scoped rule should be able to reach.
            break;
        }

        return out;
    }

} // namespace

QString RenamePlan::apply(const QString& name, const QList<RenameRule>& rules, int index)
{
    QString stem;
    QString extension;
    splitName(name, &stem, &extension);

    for (const RenameRule& rule : rules) {
        if (!rule.enabled)
            continue;

        if (rule.kind == RenameRule::Kind::Extension) {
            extension = rule.newExtension.isEmpty() ? extension.toLower() : rule.newExtension;
            continue;
        }

        switch (rule.scope) {
        case RenameRule::Scope::Stem:
            stem = applyToPart(stem, rule, index);
            break;
        case RenameRule::Scope::Extension:
            extension = applyToPart(extension, rule, index);
            break;
        case RenameRule::Scope::WholeName: {
            const QString whole = applyToPart(joinName(stem, extension), rule, index);
            splitName(whole, &stem, &extension);
            break;
        }
        }
    }

    return joinName(stem, extension);
}

RenamePlan RenamePlan::build(const QList<VfsUri>& sources, const QList<RenameRule>& rules,
    const QHash<QString, QStringList>& existingNames, Qt::CaseSensitivity sensitivity, const NameRules& names)
{
    // One spelling per name, so this layer calls a collision exactly what the
    // backend underneath will call one. On a volume that ignores case, "a.txt"
    // and "A.txt" are one name here as well as there.
    const auto key = [sensitivity](const QString& name) {
        return sensitivity == Qt::CaseSensitive ? name : name.toCaseFolded();
    };

    RenamePlan plan;

    // Names claimed so far, per directory. Two files renamed to the same thing
    // is the collision people actually hit, and the filesystem would only
    // notice it on the second one -- after the first had already moved.
    QHash<QString, QSet<QString>> claimed;

    int index = 0;
    for (const VfsUri& source : sources) {
        Entry entry;
        entry.source = source;
        entry.originalName = source.fileName();
        entry.newName = apply(entry.originalName, rules, index++);

        const QString directory = source.parent().toString();

        if (entry.newName.trimmed().isEmpty()) {
            entry.problem = QStringLiteral("the rules leave no name");
        } else if (entry.newName.startsWith(QLatin1Char('.'))
            && !entry.originalName.startsWith(QLatin1Char('.'))) {
            // "123.txt" with the digits stripped becomes ".txt" -- a legal name,
            // and almost certainly not the one anybody meant. A file that had a
            // name and would end up with only an extension is a mistake worth
            // refusing, rather than a hidden file worth creating by accident.
            entry.problem = QStringLiteral("the rules leave only an extension");
        } else if (entry.newName.contains(QLatin1Char('/')) || entry.newName.contains(QLatin1Char('\\'))) {
            // A separator would move the file rather than rename it, which is
            // not what anybody asked a rename tool to do.
            entry.problem = QStringLiteral("a name cannot contain a path separator");
        } else if (const NameVerdict verdict = checkName(entry.newName, names); verdict.isRejected()) {
            // Asked of the destination rather than assumed. The row is marked
            // before anything moves, with the offending character named, rather
            // than the run stopping part way through on an IoError that says
            // only the path.
            entry.problem = verdict.reason;
            entry.suggestion = verdict.suggestion;
        } else if (entry.changed() && claimed.value(directory).contains(key(entry.newName))) {
            entry.problem = QStringLiteral("two files would get this name");
        } else if (entry.changed()) {
            const QStringList here = existingNames.value(directory);
            // A file renamed out of the way frees its own name, so only names
            // that nothing in this batch is vacating count as taken. A rename
            // that only changes case is the smallest case of that: the file in
            // the way is the file being renamed.
            const bool present = std::any_of(here.begin(), here.end(),
                [&](const QString& name) { return key(name) == key(entry.newName); });
            const bool takenByOutsider
                = present && !std::any_of(sources.begin(), sources.end(), [&](const VfsUri& other) {
                      return other.parent().toString() == directory
                          && key(other.fileName()) == key(entry.newName);
                  });
            if (takenByOutsider)
                entry.problem = QStringLiteral("something with this name is already there");
        }

        if (!entry.isBlocked() && entry.changed())
            claimed[directory].insert(key(entry.newName));

        plan.m_entries.append(entry);
    }

    return plan;
}

int RenamePlan::changedCount() const
{
    int changed = 0;
    for (const Entry& entry : m_entries) {
        if (entry.changed())
            ++changed;
    }
    return changed;
}

int RenamePlan::blockedCount() const
{
    int blocked = 0;
    for (const Entry& entry : m_entries) {
        if (entry.isBlocked())
            ++blocked;
    }
    return blocked;
}

} // namespace mole
