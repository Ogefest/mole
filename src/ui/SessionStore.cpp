#include "ui/SessionStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace mole {

QString windowStateName(WindowState state)
{
    switch (state) {
    case WindowState::Maximized:
        return QStringLiteral("maximized");
    case WindowState::FullScreen:
        return QStringLiteral("fullscreen");
    case WindowState::Normal:
        break;
    }
    return QStringLiteral("normal");
}

WindowState windowStateFromName(const QString& name)
{
    if (name == QLatin1String("maximized"))
        return WindowState::Maximized;
    if (name == QLatin1String("fullscreen"))
        return WindowState::FullScreen;
    return WindowState::Normal;
}

namespace {

    constexpr int kSessionFormatVersion = 1;

} // namespace

SessionStore::SessionStore(QString filePath)
    : m_file(std::move(filePath))
{
}

QString SessionStore::defaultFilePath()
{
    const QByteArray override = qgetenv("MOLE_SESSION_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("session.json"));
}

bool SessionStore::save(const Session& session, QString* reasonOut)
{
    QJsonArray tabs;
    for (const TabSession& tab : session.tabs) {
        QJsonObject entry;
        entry[QStringLiteral("featureId")] = tab.featureId;
        entry[QStringLiteral("state")] = QJsonObject::fromVariantMap(tab.state);
        tabs.append(entry);
    }

    QJsonObject window;
    window[QStringLiteral("x")] = session.window.x;
    window[QStringLiteral("y")] = session.window.y;
    window[QStringLiteral("width")] = session.window.width;
    window[QStringLiteral("height")] = session.window.height;
    window[QStringLiteral("windowState")] = windowStateName(session.window.state);

    QJsonObject root;
    root[QStringLiteral("window")] = window;
    root[QStringLiteral("version")] = kSessionFormatVersion;
    root[QStringLiteral("currentIndex")] = session.currentIndex;
    root[QStringLiteral("tabs")] = tabs;

    // QSaveFile writes to a temporary and renames, so a crash half way through
    // leaves the previous session intact rather than an empty file.
    return m_file.write(QJsonDocument(root), reasonOut);
}

Session SessionStore::load() const
{
    QFile file(m_file.path());
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {};

    const QJsonObject root = document.object();
    // An unknown version means a newer build wrote it; starting fresh beats
    // guessing at a format we do not know.
    if (root.value(QStringLiteral("version")).toInt() != kSessionFormatVersion)
        return {};

    Session session;
    const QJsonArray tabs = root.value(QStringLiteral("tabs")).toArray();
    for (const QJsonValue& value : tabs) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();
        const QString featureId = entry.value(QStringLiteral("featureId")).toString();
        if (featureId.isEmpty())
            continue;
        session.tabs.append(
            TabSession { featureId, entry.value(QStringLiteral("state")).toObject().toVariantMap() });
    }

    const QJsonObject window = root.value(QStringLiteral("window")).toObject();
    session.window.x = window.value(QStringLiteral("x")).toInt(-1);
    session.window.y = window.value(QStringLiteral("y")).toInt(-1);
    session.window.width = window.value(QStringLiteral("width")).toInt(0);
    session.window.height = window.value(QStringLiteral("height")).toInt(0);
    // The tri-state replaced a "maximized" boolean, and a session written by an
    // older build is still worth reading -- somebody upgrading should not lose
    // the window they left maximised. The old key is only consulted when the new
    // one is absent, so a file carrying both is not ambiguous.
    if (window.contains(QStringLiteral("windowState"))) {
        session.window.state = windowStateFromName(window.value(QStringLiteral("windowState")).toString());
    } else if (window.value(QStringLiteral("maximized")).toBool()) {
        session.window.state = WindowState::Maximized;
    }

    session.currentIndex = root.value(QStringLiteral("currentIndex")).toInt(-1);
    if (session.currentIndex >= session.tabs.size())
        session.currentIndex = session.tabs.isEmpty() ? -1 : 0;

    return session;
}

void SessionStore::clear() const
{
    QFile::remove(m_file.path());
}

} // namespace mole
