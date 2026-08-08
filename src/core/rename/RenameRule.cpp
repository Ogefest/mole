#include "core/rename/RenameRule.h"

namespace mole {

QString RenameRule::kindToString(Kind kind)
{
    switch (kind) {
    case Kind::Replace:
        return QStringLiteral("replace");
    case Kind::Case:
        return QStringLiteral("case");
    case Kind::Insert:
        return QStringLiteral("insert");
    case Kind::Remove:
        return QStringLiteral("remove");
    case Kind::Strip:
        return QStringLiteral("strip");
    case Kind::Number:
        return QStringLiteral("number");
    case Kind::Affix:
        return QStringLiteral("affix");
    case Kind::Extension:
        return QStringLiteral("extension");
    }
    return QStringLiteral("replace");
}

RenameRule::Kind RenameRule::kindFromString(const QString& text)
{
    for (Kind kind : allKinds()) {
        if (kindToString(kind) == text)
            return kind;
    }
    return Kind::Replace;
}

QString RenameRule::kindLabel(Kind kind)
{
    switch (kind) {
    case Kind::Replace:
        return QStringLiteral("Replace text");
    case Kind::Case:
        return QStringLiteral("Change case");
    case Kind::Insert:
        return QStringLiteral("Insert text");
    case Kind::Remove:
        return QStringLiteral("Remove characters");
    case Kind::Strip:
        return QStringLiteral("Strip characters");
    case Kind::Number:
        return QStringLiteral("Number them");
    case Kind::Affix:
        return QStringLiteral("Add prefix or suffix");
    case Kind::Extension:
        return QStringLiteral("Change extension");
    }
    return {};
}

QList<RenameRule::Kind> RenameRule::allKinds()
{
    return { Kind::Replace, Kind::Case, Kind::Insert, Kind::Remove, Kind::Strip, Kind::Number, Kind::Affix,
        Kind::Extension };
}

QString RenameRule::describe() const
{
    switch (kind) {
    case Kind::Replace:
        if (find.isEmpty())
            return QStringLiteral("replace nothing");
        return QStringLiteral("replace %1\"%2\" with \"%3\"")
            .arg(useRegex ? QStringLiteral("pattern ") : QString(), find, replaceWith);
    case Kind::Case:
        switch (caseStyle) {
        case CaseStyle::Upper:
            return QStringLiteral("UPPER CASE");
        case CaseStyle::Lower:
            return QStringLiteral("lower case");
        case CaseStyle::Title:
            return QStringLiteral("Title Case");
        case CaseStyle::Sentence:
            return QStringLiteral("Sentence case");
        }
        break;
    case Kind::Insert:
        return QStringLiteral("insert \"%1\" at %2").arg(text).arg(position);
    case Kind::Remove:
        return QStringLiteral("remove %1 characters at %2").arg(length).arg(position);
    case Kind::Strip:
        switch (stripClass) {
        case StripClass::Digits:
            return QStringLiteral("strip digits");
        case StripClass::Punctuation:
            return QStringLiteral("strip punctuation");
        case StripClass::Whitespace:
            return QStringLiteral("strip spaces");
        case StripClass::Accents:
            return QStringLiteral("strip accents");
        case StripClass::NonAscii:
            return QStringLiteral("strip non-ASCII");
        }
        break;
    case Kind::Number:
        return QStringLiteral("number from %1, step %2, %3 digits").arg(start).arg(step).arg(padding);
    case Kind::Affix:
        if (prefix.isEmpty() && suffix.isEmpty())
            return QStringLiteral("add nothing");
        if (suffix.isEmpty())
            return QStringLiteral("prefix \"%1\"").arg(prefix);
        if (prefix.isEmpty())
            return QStringLiteral("suffix \"%1\"").arg(suffix);
        return QStringLiteral("prefix \"%1\", suffix \"%2\"").arg(prefix, suffix);
    case Kind::Extension:
        return newExtension.isEmpty() ? QStringLiteral("lower-case the extension")
                                      : QStringLiteral("extension \".%1\"").arg(newExtension);
    }
    return {};
}

QJsonObject RenameRule::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("kind")] = kindToString(kind);
    json[QStringLiteral("scope")] = static_cast<int>(scope);
    json[QStringLiteral("enabled")] = enabled;
    json[QStringLiteral("find")] = find;
    json[QStringLiteral("replaceWith")] = replaceWith;
    json[QStringLiteral("useRegex")] = useRegex;
    json[QStringLiteral("caseSensitive")] = caseSensitive;
    json[QStringLiteral("caseStyle")] = static_cast<int>(caseStyle);
    json[QStringLiteral("position")] = position;
    json[QStringLiteral("length")] = length;
    json[QStringLiteral("text")] = text;
    json[QStringLiteral("stripClass")] = static_cast<int>(stripClass);
    json[QStringLiteral("start")] = start;
    json[QStringLiteral("step")] = step;
    json[QStringLiteral("padding")] = padding;
    json[QStringLiteral("numberAt")] = numberAt;
    json[QStringLiteral("numberSeparator")] = numberSeparator;
    json[QStringLiteral("prefix")] = prefix;
    json[QStringLiteral("suffix")] = suffix;
    json[QStringLiteral("newExtension")] = newExtension;
    return json;
}

RenameRule RenameRule::fromJson(const QJsonObject& json)
{
    RenameRule rule;
    rule.kind = kindFromString(json.value(QStringLiteral("kind")).toString());
    rule.scope = static_cast<Scope>(json.value(QStringLiteral("scope")).toInt());
    rule.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    rule.find = json.value(QStringLiteral("find")).toString();
    rule.replaceWith = json.value(QStringLiteral("replaceWith")).toString();
    rule.useRegex = json.value(QStringLiteral("useRegex")).toBool();
    rule.caseSensitive = json.value(QStringLiteral("caseSensitive")).toBool();
    rule.caseStyle = static_cast<CaseStyle>(json.value(QStringLiteral("caseStyle")).toInt());
    rule.position = json.value(QStringLiteral("position")).toInt();
    rule.length = json.value(QStringLiteral("length")).toInt();
    rule.text = json.value(QStringLiteral("text")).toString();
    rule.stripClass = static_cast<StripClass>(json.value(QStringLiteral("stripClass")).toInt());
    rule.start = json.value(QStringLiteral("start")).toInt(1);
    rule.step = json.value(QStringLiteral("step")).toInt(1);
    rule.padding = json.value(QStringLiteral("padding")).toInt(3);
    rule.numberAt = json.value(QStringLiteral("numberAt")).toInt(-1);
    rule.numberSeparator = json.value(QStringLiteral("numberSeparator")).toString();
    rule.prefix = json.value(QStringLiteral("prefix")).toString();
    rule.suffix = json.value(QStringLiteral("suffix")).toString();
    rule.newExtension = json.value(QStringLiteral("newExtension")).toString();
    return rule;
}

} // namespace mole
