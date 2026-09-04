#include "core/vfs/NameRules.h"

#include <QSet>

namespace mole {
namespace {

    /// The MS-DOS device names, still reserved forty years later. Matched
    /// against the stem -- the part before the first dot -- because "nul.txt"
    /// opens the device just as "nul" does.
    bool isReservedDeviceName(const QString& name)
    {
        static const QSet<QString> reserved = [] {
            QSet<QString> out { QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"),
                QStringLiteral("nul") };
            for (int i = 1; i <= 9; ++i) {
                out.insert(QStringLiteral("com%1").arg(i));
                out.insert(QStringLiteral("lpt%1").arg(i));
            }
            return out;
        }();

        const int dot = name.indexOf(QLatin1Char('.'));
        const QString stem = dot < 0 ? name : name.left(dot);
        return reserved.contains(stem.toLower());
    }

    QString describe(QChar character)
    {
        if (character.unicode() < 0x20) {
            return QStringLiteral("a control character (0x%1)")
                .arg(QString::number(character.unicode(), 16).rightJustified(2, QLatin1Char('0')));
        }
        return QStringLiteral("'%1'").arg(character);
    }

} // namespace

NameRules NameRules::forPlatform(HostPlatform platform)
{
    NameRules rules;
    if (platform != HostPlatform::Windows) {
        // A POSIX filesystem refuses a separator and a null and nothing else,
        // both of which checkName() refuses whatever the rules say. A quote, a
        // newline and a question mark are all legal, and the awkward-names suite
        // is there because they are.
        // Bytes, not characters: every filesystem in use on Linux and macOS
        // counts the encoded length. maximumLength stays zero here because
        // there is no character limit to state -- 255 bytes is the whole rule.
        rules.maximumLengthInBytes = 255;
        return rules;
    }

    // The backslash is in here because it is a separator there: a file called
    // back\slash.txt, copied off a Linux disk, would not be a badly named file
    // on Windows but a file called slash.txt inside a directory called back.
    rules.forbiddenCharacters = QStringLiteral("<>:\"|?*\\");
    rules.refusesControlCharacters = true;
    rules.refusesTrailingDotOrSpace = true;
    rules.refusesReservedDeviceNames = true;
    // NTFS counts UTF-16 code units, which is what QString::size() answers.
    rules.maximumLength = 255;
    return rules;
}

NameVerdict checkName(const QString& name, const NameRules& rules)
{
    const auto reject = [](const QString& reason, const QString& suggestion) {
        NameVerdict out;
        out.accepted = false;
        out.reason = reason;
        out.suggestion = suggestion;
        return out;
    };

    // No rule set makes these usable, so they are refused before the rules are
    // consulted at all.
    if (name.isEmpty())
        return reject(QStringLiteral("a name cannot be empty"), QString());
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return reject(QStringLiteral("\"%1\" is not a name").arg(name), QString());
    if (name.contains(QLatin1Char('/'))) {
        return reject(QStringLiteral("a name cannot contain a path separator"),
            QString(name).replace(QLatin1Char('/'), QLatin1Char('_')));
    }
    if (name.contains(QChar(u'\0')))
        return reject(QStringLiteral("a name cannot contain a null"), QString());

    // Built as the reasons are found, so a caller offering it gets one name back
    // rather than one per problem.
    QString repaired = name;
    for (const QChar forbidden : rules.forbiddenCharacters)
        repaired.replace(forbidden, QLatin1Char('_'));
    if (rules.refusesControlCharacters) {
        for (int i = 0; i < repaired.size(); ++i) {
            if (repaired.at(i).unicode() < 0x20)
                repaired[i] = QLatin1Char('_');
        }
    }
    if (rules.refusesTrailingDotOrSpace) {
        while (repaired.endsWith(QLatin1Char('.')) || repaired.endsWith(QLatin1Char(' ')))
            repaired.chop(1);
    }
    if (rules.refusesReservedDeviceNames && isReservedDeviceName(repaired)) {
        const int dot = repaired.indexOf(QLatin1Char('.'));
        repaired = dot < 0 ? repaired + QLatin1Char('_')
                           : repaired.left(dot) + QLatin1Char('_') + repaired.mid(dot);
    }
    if (rules.maximumLength > 0 && repaired.size() > rules.maximumLength)
        repaired.truncate(rules.maximumLength);
    // Trimmed a character at a time rather than by arithmetic on the byte count:
    // cutting UTF-8 to a byte length lands in the middle of a character.
    while (rules.maximumLengthInBytes > 0 && repaired.toUtf8().size() > rules.maximumLengthInBytes
        && !repaired.isEmpty()) {
        repaired.chop(1);
    }
    if (repaired.isEmpty())
        repaired = QStringLiteral("unnamed");

    // Reported one reason at a time, naming the character, because "this name is
    // invalid" tells somebody staring at a hundred rows nothing they can act on.
    for (const QChar character : name) {
        if (rules.forbiddenCharacters.contains(character)) {
            return reject(
                QStringLiteral("this drive will not accept %1 in a name").arg(describe(character)), repaired);
        }
        if (rules.refusesControlCharacters && character.unicode() < 0x20) {
            return reject(
                QStringLiteral("this drive will not accept %1 in a name").arg(describe(character)), repaired);
        }
    }

    if (rules.refusesTrailingDotOrSpace
        && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))) {
        return reject(
            QStringLiteral("this drive will not accept a name ending in %1")
                .arg(name.endsWith(QLatin1Char('.')) ? QStringLiteral("a dot") : QStringLiteral("a space")),
            repaired);
    }

    if (rules.refusesReservedDeviceNames && isReservedDeviceName(name)) {
        const int dot = name.indexOf(QLatin1Char('.'));
        return reject(QStringLiteral("\"%1\" is a reserved device name on this drive")
                          .arg(dot < 0 ? name : name.left(dot)),
            repaired);
    }

    if (rules.maximumLength > 0 && name.size() > rules.maximumLength) {
        return reject(
            QStringLiteral("this drive allows %1 characters in a name").arg(rules.maximumLength), repaired);
    }

    if (rules.maximumLengthInBytes > 0 && name.toUtf8().size() > rules.maximumLengthInBytes) {
        // The count is said in bytes because that is what the limit is: "255
        // characters" would be a sentence the person could measure against and
        // still be refused.
        return reject(QStringLiteral("this drive allows %1 bytes in a name, and this one is %2")
                          .arg(rules.maximumLengthInBytes)
                          .arg(name.toUtf8().size()),
            repaired);
    }

    return {};
}

} // namespace mole
