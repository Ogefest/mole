#include "core/data/SqliteConnection.h"

#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <QVariant>

namespace mole::sqlite {
namespace {

    /// Whether this names SQLite's in-memory database rather than a file.
    bool isInMemory(const QString& path)
    {
        return path == QLatin1String(":memory:");
    }

} // namespace

Connection::Connection(QString path, Settings settings)
    : m_path(std::move(path))
    , m_settings(std::move(settings))
    , m_token(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

Connection::~Connection()
{
    closeAll();
}

QString Connection::settle(QSqlDatabase& database) const
{
    QSqlQuery pragma(database);

    // Read back rather than executed and forgotten. journal_mode is the one
    // pragma whose refusal changes behaviour instead of failing, so it is asked
    // as a question: SQLite answers with the mode it ended up in.
    if (m_settings.requireWal && !isInMemory(m_path)) {
        if (!pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL")))
            return pragma.lastError().text();
        if (!pragma.next())
            return QStringLiteral("this build of SQLite would not say what journal it is using");
        const QString mode = pragma.value(0).toString().toLower();
        if (mode != QLatin1String("wal")) {
            return QStringLiteral("this filesystem will not run the database in WAL mode (it answered "
                                  "\"%1\"), so a reader would queue behind every write")
                .arg(mode);
        }
    }

    if (m_settings.busyTimeoutMs > 0) {
        if (!pragma.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(m_settings.busyTimeoutMs)))
            return pragma.lastError().text();
    }

    for (const QString& statement : m_settings.pragmas) {
        if (!pragma.exec(statement))
            return pragma.lastError().text();
    }
    return {};
}

QSqlDatabase Connection::forCurrentThread() const
{
    // Its own lock, held for a hash lookup and a connection setup and nothing
    // else. Sharing one with the SQL was the fault ADR-0065 describes: a reader
    // queued behind a scan's transactions for want of a QHash read.
    QMutexLocker lock(&m_registry);

    QThread* const thread = QThread::currentThread();

    const auto cached = m_connections.constFind(thread);
    if (cached != m_connections.constEnd()) {
        QSqlDatabase existing = QSqlDatabase::database(*cached, false);
        if (existing.isValid() && existing.isOpen())
            return existing;
        // The thread whose connection this was has ended and its address has
        // been handed to another, so the connection it left behind is closed
        // here -- by name, which is what removeDatabase() wants from a thread
        // that does not own it. Nothing else would ever close it: there is no
        // hook on the thread any more, and the thread it belonged to is gone.
        const QString abandoned = *cached;
        m_connections.erase(cached);
        if (QSqlDatabase::contains(abandoned))
            QSqlDatabase::removeDatabase(abandoned);
    }

    const QString name = QStringLiteral("mole-sqlite-%1-%2").arg(m_token).arg(m_nextConnection++);
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    if (m_settings.readOnly) {
        database.setDatabaseName(
            QStringLiteral("file:%1?mode=ro").arg(QString::fromUtf8(QUrl::toPercentEncoding(m_path, "/"))));
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI"));
    } else {
        database.setDatabaseName(m_path);
    }

    if (!database.open()) {
        m_lastError = database.lastError().text();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        return {};
    }

    if (const QString wrong = settle(database); !wrong.isEmpty()) {
        m_lastError = wrong;
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
        return {};
    }

    // **Only for a thread Qt started, and telling them apart is the fix.**
    //
    // A connection is closed when its thread ends, which `QThread::finished`
    // says exactly -- for a thread Qt started. For an adopted one, a plain
    // `std::thread` that called into Qt, it is never emitted at all: the close
    // never ran, and the `connect()` itself held Qt's per-thread data for that
    // thread alive for the life of the process. Measured at 776 bytes in eight
    // allocations for a thread that does nothing but adopt itself and connect,
    // which is what made `make asan` red on tst_IndexDatabase and
    // tst_DelimitedStore. Adopting a thread leaks nothing by itself; connecting
    // to a signal it will never emit is the whole of it.
    //
    // The question "did Qt start this thread" has a public answer: Qt gives an
    // adopted thread's QThread object an affinity to itself, where one it
    // started lives in the thread that created it. A thread_local destructor was
    // the other way to close on any thread and is worse -- on a pooled QThread
    // it runs after Qt has taken that thread's objects down, and closing a
    // QSqlDatabase there segfaults inside the sqlite driver.
    //
    // What an adopted thread costs instead is a connection held until this
    // object is destroyed or the thread's address is handed to another, which
    // the branch above notices. See MOLE-410.
    const bool qtStartedThisThread = thread->thread() != thread;
    if (qtStartedThisThread) {
        // Captures the name and nothing else, so it holds no pointer to this
        // object and cannot outlive one; the context object is the thread, so Qt
        // drops the connection if the QThread itself goes first. Direct, because
        // the removal has to happen on the thread that owns the database --
        // finished() is emitted there, just before it exits.
        QObject::connect(
            thread, &QThread::finished, thread,
            [name] {
                QSqlDatabase leaving = QSqlDatabase::database(name, false);
                if (leaving.isValid() && leaving.isOpen())
                    leaving.close();
                leaving = QSqlDatabase();
                QSqlDatabase::removeDatabase(name);
            },
            Qt::DirectConnection);
    }

    m_connections.insert(thread, name);
    m_lastError.clear();
    return database;
}

QString Connection::lastError() const
{
    QMutexLocker lock(&m_registry);
    return m_lastError;
}

void Connection::closeAll()
{
    QMutexLocker lock(&m_registry);
    QThread* const self = QThread::currentThread();
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it) {
        if (it.key() == self) {
            QSqlDatabase mine = QSqlDatabase::database(it.value(), false);
            if (mine.isValid())
                mine.close();
        }
        // A connection belonging to another thread is dropped by name alone.
        // Fetching the QSqlDatabase object for it from here is precisely what
        // Qt warns about, and it buys nothing: removeDatabase() closes it.
        QSqlDatabase::removeDatabase(it.value());
    }
    m_connections.clear();
}

} // namespace mole::sqlite
