#include "plugins/rclone/RcloneFactory.h"

#include "plugins/rclone/RcloneFileSystem.h"
#include "plugins/rclone/RcloneLibrary.h"

#include <QJsonArray>
#include <QJsonObject>

namespace mole {
namespace {

    /// rclone options that describe the tool rather than the connection. Offering
    /// them in a mount form would be noise.
    bool isBoringOption(const QString& name)
    {
        static const QStringList boring = { QStringLiteral("encoding"), QStringLiteral("description") };
        return boring.contains(name);
    }

    ConnectionField::Kind kindFor(const QJsonObject& option)
    {
        if (option.value(QStringLiteral("IsPassword")).toBool())
            return ConnectionField::Password;

        const QJsonArray examples = option.value(QStringLiteral("Examples")).toArray();
        const QString type = option.value(QStringLiteral("Type")).toString();

        if (type == QLatin1String("bool"))
            return ConnectionField::Boolean;
        if (type == QLatin1String("int") || type == QLatin1String("SizeSuffix")
            || type == QLatin1String("Duration")) {
            return ConnectionField::Number;
        }
        // Examples are suggestions in rclone, but for a field like `provider` they
        // are the whole set, and a list beats free text every time.
        if (!examples.isEmpty() && !option.value(QStringLiteral("ExclusiveExamples")).isNull())
            return ConnectionField::Choice;
        if (!examples.isEmpty() && examples.size() <= 24)
            return ConnectionField::Choice;
        return ConnectionField::Text;
    }

    /// The first line of rclone's help, which is the part that fits in a form.
    QString shortHelp(const QString& help)
    {
        const int newline = help.indexOf(QLatin1Char('\n'));
        QString first = newline < 0 ? help : help.left(newline);
        return first.trimmed();
    }

} // namespace

bool RcloneFactory::isAvailable() const
{
    return RcloneLibrary::instance().isAvailable();
}

QString RcloneFactory::unavailableReason() const
{
    return RcloneLibrary::instance().unavailableReason();
}

QList<BackendVariant> RcloneFactory::variants() const
{
    if (!m_variants.isEmpty())
        return m_variants;
    if (!isAvailable())
        return {};

    QString error;
    const QJsonObject reply = RcloneLibrary::instance().call(QStringLiteral("config/providers"), {}, &error);
    if (!error.isEmpty())
        return {};

    const QJsonArray providers = reply.value(QStringLiteral("providers")).toArray();
    for (const QJsonValue& value : providers) {
        const QJsonObject provider = value.toObject();

        BackendVariant variant;
        variant.id = provider.value(QStringLiteral("Name")).toString();
        variant.label = provider.value(QStringLiteral("Name")).toString();
        variant.description = provider.value(QStringLiteral("Description")).toString();

        // "local" is the disk this application already reads directly, and
        // going through rclone to reach it would be slower for no gain.
        if (variant.id == QLatin1String("local") || variant.id.isEmpty())
            continue;

        const QJsonArray options = provider.value(QStringLiteral("Options")).toArray();
        for (const QJsonValue& optionValue : options) {
            const QJsonObject option = optionValue.toObject();
            const QString name = option.value(QStringLiteral("Name")).toString();
            if (name.isEmpty() || isBoringOption(name))
                continue;

            ConnectionField field;
            field.key = name;
            field.label = name;
            field.kind = kindFor(option);
            field.help = shortHelp(option.value(QStringLiteral("Help")).toString());
            field.required = option.value(QStringLiteral("Required")).toBool();
            field.advanced = option.value(QStringLiteral("Advanced")).toBool();
            field.defaultValue = option.value(QStringLiteral("Default")).toVariant();

            const QJsonArray examples = option.value(QStringLiteral("Examples")).toArray();
            for (const QJsonValue& exampleValue : examples) {
                const QJsonObject example = exampleValue.toObject();
                field.choices.append(example.value(QStringLiteral("Value")).toString());
                const QString help = shortHelp(example.value(QStringLiteral("Help")).toString());
                field.choiceLabels.append(
                    help.isEmpty() ? example.value(QStringLiteral("Value")).toString() : help);
            }

            // S3 asks entirely different questions for AWS and for Ceph, and
            // rclone says which by tagging the option with the providers it
            // belongs to. Showing all eighty at once would be unusable.
            const QString provider = option.value(QStringLiteral("Provider")).toString();
            if (!provider.isEmpty() && !provider.startsWith(QLatin1Char('!'))) {
                field.dependsOnKey = QStringLiteral("provider");
                field.dependsOnValues = provider.split(QLatin1Char(','), Qt::SkipEmptyParts);
            }

            variant.fields.append(field);
        }

        m_variants.append(variant);
    }

    return m_variants;
}

QString RcloneFactory::connectionStringFor(const QString& backend, const QVariantMap& config)
{
    // rclone's connection-string form: `:backend,key=value,key=value:`.
    //
    // A value containing a comma or a quote would otherwise end the parameter
    // list early and connect somewhere else entirely -- with a password, that
    // means sending it to the wrong host. rclone's own rule is to wrap in
    // double quotes and double any inside.
    const auto quote = [](const QString& value) {
        if (!value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('"'))
            && !value.contains(QLatin1Char(':')) && !value.contains(QLatin1Char(' '))) {
            return value;
        }
        QString escaped = value;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    };

    QStringList parameters;
    for (auto it = config.constBegin(); it != config.constEnd(); ++it) {
        // Keys the host adds for its own bookkeeping are not rclone options.
        if (it.key().startsWith(QLatin1String("__")))
            continue;
        const QString value = it.value().toString();
        if (value.isEmpty())
            continue;
        parameters.append(QStringLiteral("%1=%2").arg(it.key(), quote(value)));
    }

    QString out = QLatin1Char(':') + backend;
    if (!parameters.isEmpty()) {
        // Sorted, so the same configuration always produces the same string --
        // which makes it comparable, cacheable and testable.
        parameters.sort();
        out += QLatin1Char(',') + parameters.join(QLatin1Char(','));
    }
    return out + QLatin1Char(':');
}

FileSystemPtr RcloneFactory::create(const QVariantMap& config, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return FileSystemPtr {};
    };

    if (!isAvailable())
        return fail(unavailableReason());

    const QString backend = config.value(variantKey()).toString();
    if (backend.isEmpty())
        return fail(QStringLiteral("No backend was chosen"));

    const QString root = config.value(QStringLiteral("__root")).toString();
    const QString scheme = config.value(QStringLiteral("__scheme"), QStringLiteral("rclone")).toString();

    return std::make_shared<RcloneFileSystem>(scheme, connectionStringFor(backend, config), root);
}

} // namespace mole
