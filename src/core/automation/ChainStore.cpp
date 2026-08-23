#include "core/automation/ChainStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace mole {

ChainStore::ChainStore(QString path, QObject* parent)
    : QObject(parent)
    , m_path(std::move(path))
{
}

QString ChainStore::defaultPath()
{
    const QByteArray override = qgetenv("MOLE_CHAINS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("chains.json"));
}

bool ChainStore::load()
{
    m_chains.clear();
    m_unreadable = 0;

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return false; // no chains yet is not an error

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    const QJsonArray stored = document.object().value(QStringLiteral("chains")).toArray();
    for (const QJsonValue& value : stored) {
        const std::optional<Chain> chain = Chain::fromJson(value.toObject());
        if (!chain || chain->id.isEmpty()) {
            ++m_unreadable;
            continue;
        }
        m_chains.append(*chain);
    }
    emit chainsChanged();
    return true;
}

bool ChainStore::save() const
{
    const QFileInfo info(m_path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

    QJsonArray stored;
    for (const Chain& chain : m_chains)
        stored.append(chain.toJson());

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("chains")] = stored;

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

Chain ChainStore::chain(const QString& id) const
{
    for (const Chain& chain : m_chains) {
        if (chain.id == id)
            return chain;
    }
    return {};
}

bool ChainStore::put(const Chain& chain)
{
    if (chain.id.isEmpty())
        return false;
    for (int i = 0; i < m_chains.size(); ++i) {
        if (m_chains.at(i).id == chain.id) {
            // In place, so editing a chain does not move it to the end of
            // somebody's list.
            m_chains[i] = chain;
            emit chainsChanged();
            return true;
        }
    }
    m_chains.append(chain);
    emit chainsChanged();
    return true;
}

bool ChainStore::remove(const QString& id)
{
    for (int i = 0; i < m_chains.size(); ++i) {
        if (m_chains.at(i).id == id) {
            m_chains.removeAt(i);
            emit chainsChanged();
            return true;
        }
    }
    return false;
}

} // namespace mole
