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
    : JsonFileStore(std::move(path), parent)
{
}

QString ChainStore::defaultPath()
{
    return pathFor("MOLE_CHAINS_PATH", QStringLiteral("chains.json"));
}

bool ChainStore::load()
{
    QJsonObject root;
    const Read read = readRoot(&root);
    if (read == Read::Damaged)
        return false; // kept, and nothing is written over it until somebody says

    m_chains.clear();
    m_unreadable = 0;
    if (read == Read::Missing) {
        emit chainsChanged();
        return true; // nothing saved yet is the ordinary first run
    }

    const QJsonArray stored = root.value(QStringLiteral("chains")).toArray();
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

bool ChainStore::save()
{
    QJsonArray stored;
    for (const Chain& chain : m_chains)
        stored.append(chain.toJson());

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("chains")] = stored;

    return writeRoot(root);
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
