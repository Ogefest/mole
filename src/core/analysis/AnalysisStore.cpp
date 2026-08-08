#include "core/analysis/AnalysisStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace mole {

AnalysisStore::AnalysisStore(QString directory)
    : m_directory(std::move(directory))
{
}

QString AnalysisStore::defaultDirectory()
{
    const QByteArray override = qgetenv("MOLE_ANALYSIS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("analysis"));
}

QString AnalysisStore::folderNameFor(const QString& rootUri)
{
    // A hash rather than the path: uris contain slashes and colons, can be
    // longer than a filename may be, and two different drives can hold the
    // same path. A readable prefix keeps the directory browsable by hand.
    const QByteArray digest
        = QCryptographicHash::hash(rootUri.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);

    QString readable = rootUri;
    readable.remove(QRegularExpression(QStringLiteral("^[a-z]+://")));
    readable.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    readable = readable.right(40);

    return QStringLiteral("%1-%2").arg(QString::fromLatin1(digest), readable);
}

QString AnalysisStore::folderFor(const QString& rootUri) const
{
    return QDir(m_directory).filePath(folderNameFor(rootUri));
}

bool AnalysisStore::save(const AnalysisReport& report) const
{
    if (!report.isValid())
        return false;

    const QString folder = folderFor(report.rootUri);
    if (!QDir().mkpath(folder))
        return false;

    QSaveFile file(QDir(folder).filePath(report.id + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(report.toJson()).toJson(QJsonDocument::Indented));
    return file.commit();
}

QList<ReportSummary> AnalysisStore::history(const QString& rootUri) const
{
    QList<ReportSummary> out;

    QDir folder(folderFor(rootUri));
    if (!folder.exists())
        return out;

    const QStringList files = folder.entryList({ QStringLiteral("*.json") }, QDir::Files);
    for (const QString& name : files) {
        QFile file(folder.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;

        QJsonParseError error {};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            continue; // one bad file must not hide the rest of the history

        const AnalysisReport report = AnalysisReport::fromJson(document.object());
        if (!report.isValid())
            continue;

        out.append(ReportSummary {
            report.id, report.rootUri, report.label, report.createdAt, report.fileCount, report.totalBytes });
    }

    // The id breaks ties: timestamps are stored to the second, so two runs in
    // the same second would otherwise come back in an arbitrary order.
    std::sort(out.begin(), out.end(), [](const ReportSummary& a, const ReportSummary& b) {
        if (a.createdAt != b.createdAt)
            return a.createdAt > b.createdAt;
        return a.id > b.id;
    });
    return out;
}

QSet<QString> AnalysisStore::storedFolderNames() const
{
    QSet<QString> names;
    QDir base(m_directory);
    if (!base.exists())
        return names;

    const QStringList folders = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& folder : folders) {
        // An empty folder is one whose reports were all deleted; claiming a
        // report exists there would offer to open nothing.
        if (!QDir(base.filePath(folder)).entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty())
            names.insert(folder);
    }
    return names;
}

QStringList AnalysisStore::analysedRoots() const
{
    QStringList roots;
    QDir base(m_directory);
    if (!base.exists())
        return roots;

    const QStringList folders = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& folder : folders) {
        QDir reports(base.filePath(folder));
        const QStringList files = reports.entryList({ QStringLiteral("*.json") }, QDir::Files);
        if (files.isEmpty())
            continue;

        QFile file(reports.filePath(files.first()));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        const QString root = document.object().value(QStringLiteral("rootUri")).toString();
        if (!root.isEmpty())
            roots.append(root);
    }

    roots.sort();
    return roots;
}

AnalysisReport AnalysisStore::load(const QString& rootUri, const QString& id) const
{
    QFile file(QDir(folderFor(rootUri)).filePath(id + QStringLiteral(".json")));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {};

    return AnalysisReport::fromJson(document.object());
}

AnalysisReport AnalysisStore::latest(const QString& rootUri) const
{
    const QList<ReportSummary> summaries = history(rootUri);
    if (summaries.isEmpty())
        return {};
    return load(rootUri, summaries.first().id);
}

bool AnalysisStore::remove(const QString& rootUri, const QString& id) const
{
    return QFile::remove(QDir(folderFor(rootUri)).filePath(id + QStringLiteral(".json")));
}

int AnalysisStore::prune(const QString& rootUri, int keep) const
{
    if (keep < 0)
        return 0;

    const QList<ReportSummary> summaries = history(rootUri);
    int removed = 0;
    for (int i = keep; i < summaries.size(); ++i) {
        if (remove(rootUri, summaries.at(i).id))
            ++removed;
    }
    return removed;
}

} // namespace mole
