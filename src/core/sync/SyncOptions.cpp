#include "core/sync/SyncOptions.h"

#include <QRegularExpression>

namespace mole {

QString SyncOptions::modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Update:
        return QStringLiteral("update");
    case Mode::Mirror:
        return QStringLiteral("mirror");
    case Mode::FillGaps:
        return QStringLiteral("fill");
    }
    return QStringLiteral("update");
}

SyncOptions::Mode SyncOptions::modeFromString(const QString& text)
{
    if (text == QLatin1String("mirror"))
        return Mode::Mirror;
    if (text == QLatin1String("fill"))
        return Mode::FillGaps;
    return Mode::Update;
}

QString SyncOptions::modeLabel(Mode mode)
{
    switch (mode) {
    case Mode::Update:
        return QStringLiteral("Update");
    case Mode::Mirror:
        return QStringLiteral("Mirror");
    case Mode::FillGaps:
        return QStringLiteral("Fill gaps");
    }
    return {};
}

QString SyncOptions::modeDescription(Mode mode)
{
    switch (mode) {
    case Mode::Update:
        return QStringLiteral("Copies what is missing or has changed. Nothing at the "
                              "destination is ever removed.");
    case Mode::Mirror:
        return QStringLiteral("Makes the destination match the source exactly, including "
                              "deleting what the source does not have. Try it as a dry run "
                              "first.");
    case Mode::FillGaps:
        return QStringLiteral("Copies only what is missing. Files already there are left "
                              "alone however old they are.");
    }
    return {};
}

QString SyncOptions::compareToString(Compare compare)
{
    switch (compare) {
    case Compare::SizeAndTime:
        return QStringLiteral("size+time");
    case Compare::SizeOnly:
        return QStringLiteral("size");
    case Compare::Contents:
        return QStringLiteral("contents");
    }
    return QStringLiteral("size+time");
}

SyncOptions::Compare SyncOptions::compareFromString(const QString& text)
{
    if (text == QLatin1String("size"))
        return Compare::SizeOnly;
    if (text == QLatin1String("contents"))
        return Compare::Contents;
    return Compare::SizeAndTime;
}

QString SyncOptions::compareLabel(Compare compare)
{
    switch (compare) {
    case Compare::SizeAndTime:
        return QStringLiteral("Size or time differs");
    case Compare::SizeOnly:
        return QStringLiteral("Size differs");
    case Compare::Contents:
        return QStringLiteral("Contents differ");
    }
    return {};
}

bool SyncOptions::accepts(const QString& name, bool hidden) const
{
    if (hidden && !includeHidden)
        return false;

    const auto matches = [&name](const QStringList& patterns) {
        for (const QString& pattern : patterns) {
            const QRegularExpression expression(QRegularExpression::wildcardToRegularExpression(pattern),
                QRegularExpression::CaseInsensitiveOption);
            if (expression.match(name).hasMatch())
                return true;
        }
        return false;
    };

    // Include wins when both match: a narrow include beside a broad exclude is
    // how people express "everything except X, but definitely Y".
    if (!includePatterns.isEmpty() && matches(includePatterns))
        return true;
    if (!excludePatterns.isEmpty() && matches(excludePatterns))
        return false;
    return includePatterns.isEmpty() || matches(includePatterns);
}

QJsonObject SyncOptions::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("mode")] = modeToString(mode);
    json[QStringLiteral("compare")] = compareToString(compare);
    json[QStringLiteral("dryRun")] = dryRun;
    json[QStringLiteral("skipNewer")] = skipNewer;
    json[QStringLiteral("recursive")] = recursive;
    json[QStringLiteral("includeHidden")] = includeHidden;
    json[QStringLiteral("include")] = includePatterns.join(QLatin1Char(';'));
    json[QStringLiteral("exclude")] = excludePatterns.join(QLatin1Char(';'));
    return json;
}

SyncOptions SyncOptions::fromJson(const QJsonObject& json)
{
    SyncOptions options;
    options.mode = modeFromString(json.value(QStringLiteral("mode")).toString());
    options.compare = compareFromString(json.value(QStringLiteral("compare")).toString());
    // Defaults to a dry run when the stored value is missing: an unattended
    // mirror is not something to fall back into.
    options.dryRun = json.value(QStringLiteral("dryRun")).toBool(true);
    options.skipNewer = json.value(QStringLiteral("skipNewer")).toBool(true);
    options.recursive = json.value(QStringLiteral("recursive")).toBool(true);
    options.includeHidden = json.value(QStringLiteral("includeHidden")).toBool(false);
    options.includePatterns
        = json.value(QStringLiteral("include")).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    options.excludePatterns
        = json.value(QStringLiteral("exclude")).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    return options;
}

} // namespace mole
