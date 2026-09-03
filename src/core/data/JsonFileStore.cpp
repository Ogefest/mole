#include "core/data/JsonFileStore.h"

#include "core/diagnostics/Diagnostics.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace mole {

JsonFile::JsonFile(QString path)
    : m_path(std::move(path))
{
}

QString JsonFile::pathFor(const char* environmentVariable, const QString& fileName)
{
    const QByteArray override = qgetenv(environmentVariable);
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(fileName);
}

void JsonFile::discardDamage()
{
    m_damaged = false;
    m_damagedCopy.clear();
}

JsonFile::Read JsonFile::readDocument(QJsonDocument* documentOut)
{
    QFile file(m_path);
    if (!file.exists())
        return Read::Missing;

    const QString name = QFileInfo(m_path).fileName();
    if (!file.open(QIODevice::ReadOnly)) {
        // There and unreadable is damage of a kind nothing here can tidy: a file
        // it cannot open is one it cannot rename either. It is still not a file
        // to write over.
        qCWarning(storeLog, "%s could not be opened: %s", qPrintable(name), qPrintable(file.errorString()));
        m_damaged = true;
        return Read::Damaged;
    }

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();
    if (error.error != QJsonParseError::NoError || document.isNull()) {
        qCWarning(storeLog, "%s could not be read: %s", qPrintable(name),
            qPrintable(error.error == QJsonParseError::NoError ? QStringLiteral("it is empty")
                                                               : error.errorString()));
        keepWhatCannotBeRead();
        m_damaged = true;
        return Read::Damaged;
    }

    m_damaged = false;
    m_damagedCopy.clear();
    if (documentOut)
        *documentOut = document;
    return Read::Loaded;
}

JsonFile::Read JsonFile::readRoot(QJsonObject* rootOut)
{
    QJsonDocument document;
    const Read read = readDocument(&document);
    if (read != Read::Loaded)
        return read;

    if (!document.isObject()) {
        qCWarning(storeLog, "%s is not what this store writes, so it was kept rather than replaced",
            qPrintable(QFileInfo(m_path).fileName()));
        keepWhatCannotBeRead();
        m_damaged = true;
        return Read::Damaged;
    }
    if (rootOut)
        *rootOut = document.object();
    return Read::Loaded;
}

JsonFile::Read JsonFile::readArray(QJsonArray* arrayOut)
{
    QJsonDocument document;
    const Read read = readDocument(&document);
    if (read != Read::Loaded)
        return read;

    if (!document.isArray()) {
        qCWarning(storeLog, "%s is not what this store writes, so it was kept rather than replaced",
            qPrintable(QFileInfo(m_path).fileName()));
        keepWhatCannotBeRead();
        m_damaged = true;
        return Read::Damaged;
    }
    if (arrayOut)
        *arrayOut = document.array();
    return Read::Loaded;
}

bool JsonFile::keepWhatCannotBeRead()
{
    // Beside it and named for when it happened, so a second bad start does not
    // overwrite the first copy -- which would be this same fault, one level up.
    const QString kept = m_path + QStringLiteral(".broken-")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmsszzz"));
    if (!QFile::rename(m_path, kept)) {
        qCWarning(storeLog, "%s could not be moved aside, so it is left where it is",
            qPrintable(QFileInfo(m_path).fileName()));
        return false;
    }
    m_damagedCopy = kept;
    return true;
}

bool JsonFile::write(const QJsonDocument& document, QString* reasonOut)
{
    const auto refuse = [reasonOut](const QString& reason) {
        qCWarning(storeLog, "%s", qPrintable(reason));
        if (reasonOut)
            *reasonOut = reason;
        return false;
    };

    const QFileInfo info(m_path);

    // Refused only while the unreadable file is still *there*. What this would
    // write is whatever was left after a load that read nothing, and putting
    // that over a file somebody built by hand is the loss this class exists to
    // stop -- but once the old one has been moved aside there is nothing left to
    // lose, and refusing for the rest of the session would leave the feature
    // stuck with only a message to explain it.
    if (m_damaged && m_damagedCopy.isEmpty()) {
        return refuse(QStringLiteral("%1 could not be read and could not be moved aside, so it is "
                                     "not being written over")
                          .arg(info.fileName()));
    }

    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return refuse(QStringLiteral("there is nowhere to write %1").arg(info.fileName()));

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return refuse(QStringLiteral("%1 could not be opened for writing: %2")
                          .arg(info.fileName(), file.errorString()));
    }

    file.write(document.toJson(QJsonDocument::Indented));
    // Where a full disk arrives: QSaveFile writes to a temporary and finds out
    // at the flush and the rename, not at the write above.
    if (!file.commit())
        return refuse(QStringLiteral("%1 could not be written: %2").arg(info.fileName(), file.errorString()));

    if (reasonOut)
        reasonOut->clear();
    return true;
}

JsonFileStore::JsonFileStore(QString path, QObject* parent)
    : QObject(parent)
    , m_file(std::move(path))
{
}

JsonFileStore::Read JsonFileStore::readRoot(QJsonObject* rootOut)
{
    const Read read = m_file.readRoot(rootOut);
    if (read == Read::Damaged && !m_file.damagedCopyPath().isEmpty())
        emit loadFoundDamage(m_file.damagedCopyPath());
    return read;
}

bool JsonFileStore::writeRoot(const QJsonObject& root)
{
    QString reason;
    if (m_file.write(QJsonDocument(root), &reason))
        return true;
    emit saveFailed(reason);
    return false;
}

} // namespace mole
