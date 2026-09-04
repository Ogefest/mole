#include "core/analysis/AnalysisStore.h"

#include "core/data/JsonFileStore.h"
#include "core/diagnostics/Diagnostics.h"
#include "core/vfs/VfsUri.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>

namespace mole {

AnalysisStore::AnalysisStore(QString directory, QObject* parent)
    : QObject(parent)
    , m_directory(std::move(directory))
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
    //
    // Hashed from the canonical spelling, not the text as typed. On a volume
    // that does not distinguish case, one folder reached two ways is one folder,
    // and hashing the spelling grew it two stores that never agreed.
    const VfsUri parsed = VfsUri::fromString(rootUri);
    const QString key = parsed.isValid() ? parsed.canonicalKey() : rootUri;
    const QByteArray digest
        = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);

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

bool AnalysisStore::save(const AnalysisReport& report)
{
    if (!report.isValid())
        return false;

    // One file per report rather than one file holding them all, so this keeps
    // its own writer -- but it says the same thing when it cannot write, through
    // the same log. See ADR-0089.
    const QString folder = folderFor(report.rootUri);
    if (!QDir().mkpath(folder)) {
        qCWarning(storeLog, "there is nowhere to write the report for this folder");
        return false;
    }

    JsonFile file(QDir(folder).filePath(report.id + QStringLiteral(".json")));
    if (!file.write(QJsonDocument(report.toJson()), nullptr))
        return false;

    // What was kept about this folder is now out of date, and whoever is showing
    // it wants to know -- including when the scheduler filed this from a job
    // nobody is looking at.
    {
        QMutexLocker hold(&m_lock);
        m_history.remove(report.rootUri);
        m_rootsKnown = false;
    }
    emit changed(report.rootUri);
    return true;
}

QList<ReportSummary> AnalysisStore::history(const QString& rootUri) const
{
    {
        QMutexLocker hold(&m_lock);
        const auto kept = m_history.constFind(rootUri);
        if (kept != m_history.constEnd())
            return *kept;
    }

    // Outside the lock: this opens and parses every report in the folder, and
    // holding the lock through it would make one slow folder stop every other
    // answer. Two callers racing here read the same directory twice, which costs
    // a read and cannot be wrong.
    checkNotOnTheDrawingThread("history");
    const QList<ReportSummary> read = readHistory(rootUri);

    QMutexLocker hold(&m_lock);
    m_history.insert(rootUri, read);
    return read;
}

void AnalysisStore::forgetSummaries()
{
    QMutexLocker hold(&m_lock);
    m_history.clear();
    m_roots.clear();
    m_rootsKnown = false;
}

void AnalysisStore::readEverything()
{
    const QStringList roots = analysedRoots();
    for (const QString& root : roots)
        (void)history(root);
}

void AnalysisStore::doNotReadFrom(QThread* thread)
{
    m_noReadsFrom = thread;
}

void AnalysisStore::checkNotOnTheDrawingThread(const char* what) const
{
    if (m_noReadsFrom.load() != QThread::currentThread())
        return;
    // Not qCWarning: a programming fault rather than an operational fact, and it
    // should be visible without anybody turning a category on. The same shape
    // IndexDatabase::checkNotOnTheDrawingThread() has.
    qWarning("Report store read on the thread that draws the window: %s. It parses every "
             "saved report -- prime it with a ReadReportSummariesTask and read what it kept. "
             "See MOLE-380.",
        what);
}

QList<ReportSummary> AnalysisStore::readHistory(const QString& rootUri) const
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
    {
        QMutexLocker hold(&m_lock);
        if (m_rootsKnown)
            return m_roots;
    }

    checkNotOnTheDrawingThread("analysedRoots");
    const QStringList roots = readRoots();

    QMutexLocker hold(&m_lock);
    m_roots = roots;
    m_rootsKnown = true;
    return roots;
}

QStringList AnalysisStore::readRoots() const
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

bool AnalysisStore::remove(const QString& rootUri, const QString& id)
{
    if (!QFile::remove(QDir(folderFor(rootUri)).filePath(id + QStringLiteral(".json"))))
        return false;
    {
        QMutexLocker hold(&m_lock);
        m_history.remove(rootUri);
        m_rootsKnown = false;
    }
    emit changed(rootUri);
    return true;
}

int AnalysisStore::prune(const QString& rootUri, int keep)
{
    if (keep < 0)
        return 0;

    const QList<ReportSummary> summaries = history(rootUri);
    int removed = 0;
    for (int i = keep; i < summaries.size(); ++i) {
        // remove() announces each one, which is one signal per pruned report on
        // a rebuild that would coalesce them anyway. Deleted by hand here so the
        // announcement is once, at the end, and only when something went.
        if (QFile::remove(QDir(folderFor(rootUri)).filePath(summaries.at(i).id + QStringLiteral(".json"))))
            ++removed;
    }
    if (removed > 0) {
        {
            QMutexLocker hold(&m_lock);
            m_history.remove(rootUri);
            m_rootsKnown = false;
        }
        emit changed(rootUri);
    }
    return removed;
}

ReadReportSummariesTask::ReadReportSummariesTask(AnalysisStore* store, QObject* parent)
    : Task(QStringLiteral("Read the saved reports"), parent)
    , m_store(store)
{
    // Housekeeping the user did not ask for: it happens once, when a tab that
    // needs the summaries opens, and it has no business in the task strip.
    setBackground(true);
}

void ReadReportSummariesTask::run()
{
    if (m_store)
        m_store->readEverything();
}

} // namespace mole
