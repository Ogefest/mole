#include "core/diagnostics/Diagnostics.h"

#include <QRegularExpression>

namespace mole {

// QtInfoMsg as the default level, not QtDebugMsg: the running commentary stays
// out of the way until it is asked for, while anything these categories call a
// warning -- a short download, a job that failed, a copy that carried fewer
// bytes than it was promised -- always reaches the session log, which is where
// it is wanted.
Q_LOGGING_CATEGORY(taskLog, "mole.task", QtInfoMsg)
Q_LOGGING_CATEGORY(driveLog, "mole.drive", QtInfoMsg)
Q_LOGGING_CATEGORY(networkLog, "mole.net", QtInfoMsg)
Q_LOGGING_CATEGORY(curlLog, "mole.curl", QtInfoMsg)
Q_LOGGING_CATEGORY(updateLog, "mole.update", QtInfoMsg)

namespace diagnostics {
    namespace {

        struct Named
        {
            const char* keyword;
            const char* category;
        };

        constexpr Named kCategories[] = {
            { "task", "mole.task" },
            { "drive", "mole.drive" },
            { "net", "mole.net" },
            { "curl", "mole.curl" },
            { "update", "mole.update" },
        };

    } // namespace

    QStringList applyEnvironment()
    {
        const QString wanted = QString::fromLocal8Bit(qgetenv("MOLE_LOG")).trimmed();
        if (wanted.isEmpty())
            return {};

        const QStringList asked
            = wanted.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);

        QStringList enabled;
        QString rules;
        for (const QString& name : asked) {
            const QString lowered = name.toLower();
            bool known = false;
            for (const Named& category : kCategories) {
                if (lowered != QLatin1String("all") && lowered != QLatin1String(category.keyword))
                    continue;
                known = true;
                const QString categoryName = QString::fromLatin1(category.category);
                if (enabled.contains(categoryName))
                    continue;
                enabled.append(categoryName);
                rules += categoryName + QStringLiteral(".debug=true\n");
            }
            if (!known) {
                qWarning("MOLE_LOG: no such log as \"%s\" -- try task, drive, net, curl, update or all",
                    qPrintable(name));
            }
        }

        if (!rules.isEmpty()) {
            // Added to whatever QT_LOGGING_RULES already said rather than replacing
            // it: the two are different ways of asking for the same thing, and
            // setting one should not silently undo the other.
            QLoggingCategory::setFilterRules(rules);
        }
        return enabled;
    }

} // namespace diagnostics
} // namespace mole
