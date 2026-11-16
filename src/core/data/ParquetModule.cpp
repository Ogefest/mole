#include "core/data/ParquetModule.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QLibraryInfo>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QVersionNumber>

namespace mole::parquetModule {

namespace {

    const QString kFileName = QStringLiteral("mole-parquet.so");

    QString qtVersionText(int version)
    {
        return QStringLiteral("%1.%2").arg((version >> 16) & 0xff).arg((version >> 8) & 0xff);
    }

    /// Every module loaded, kept for the life of the process: a reader it made may
    /// still be alive at exit. Held here rather than simply never deleted, so the
    /// handle stays reachable and a leak checker sees a library that is in use
    /// rather than memory nobody can free.
    void keepForever(QLibrary* library)
    {
        static QMutex mutex;
        static QList<QLibrary*> kept;
        QMutexLocker locker(&mutex);
        kept.append(library);
    }

    State refuse(State state, const QString& reason)
    {
        state.loaded = false;
        state.reason = reason;
        state.create = nullptr;
        return state;
    }

} // namespace

QStringList candidates()
{
    const QString fromEnv
        = QProcessEnvironment::systemEnvironment().value(QStringLiteral("MOLE_PARQUET_MODULE"));
    if (!fromEnv.isEmpty())
        return { fromEnv };

    QStringList out;
    const QString appDirPath = QCoreApplication::applicationDirPath();
    if (appDirPath.isEmpty())
        return out;
    const QDir appDir(appDirPath);
    // A build tree: the module is written to <build>/modules, beside the
    // executable, and the test binaries are one directory further down.
    out.append(QDir::cleanPath(appDir.filePath(QStringLiteral("modules/") + kFileName)));
    out.append(QDir::cleanPath(appDir.filePath(QStringLiteral("../modules/") + kFileName)));
    // An install: <prefix>/bin/mole beside <prefix>/<libdir>/mole/modules. Both
    // lib and lib64, for the reason PluginManager::defaultSearchPaths() gives --
    // GNUInstallDirs answers lib64 on every RPM distribution.
    for (const char* libdir : { "lib", "lib64" }) {
        out.append(QDir::cleanPath(
            appDir.filePath(QStringLiteral("../%1/mole/modules/").arg(QLatin1String(libdir)) + kFileName)));
    }
    out.removeDuplicates();
    return out;
}

QString explainLoadFailure(const QString& loaderMessage)
{
    // A library the module links is not on this system. On Debian 13 and Ubuntu
    // 24.04 that is the ordinary case for the .deb: Arrow is recommended, and there
    // is no Arrow there to install.
    if (loaderMessage.contains(QStringLiteral("libarrow"))
        || loaderMessage.contains(QStringLiteral("libparquet"))) {
        return QStringLiteral("the Arrow libraries it was built against are not installed (%1)")
            .arg(loaderMessage);
    }
    // The libraries are there and are older than the ones it was built against: a
    // symbol version (`GLIBC_2.42`, `GLIBCXX_3.4.34`, `Qt_6.10`) or a symbol that
    // does not exist yet. That is a module built on a newer distribution than this
    // one -- the .deb's module on an Ubuntu 24.04 that somebody gave Apache's
    // Arrow -- and it is said as that, not as a missing file.
    if (loaderMessage.contains(QStringLiteral("not found"))
        && loaderMessage.contains(QStringLiteral("version"))) {
        return QStringLiteral("it was built for a newer system than this one (%1)").arg(loaderMessage);
    }
    if (loaderMessage.contains(QStringLiteral("undefined symbol"))) {
        return QStringLiteral("it was built for a newer system than this one (%1)").arg(loaderMessage);
    }
    if (loaderMessage.contains(QStringLiteral("cannot open shared object file"))) {
        return QStringLiteral("a library it needs is not installed (%1)").arg(loaderMessage);
    }
    return QStringLiteral("it could not be loaded (%1)").arg(loaderMessage);
}

State loadFrom(const QStringList& candidateList)
{
    State state;
    for (const QString& candidate : candidateList) {
        if (QFileInfo(candidate).isFile()) {
            state.path = candidate;
            break;
        }
    }
    if (state.path.isEmpty()) {
        return refuse(state,
            QStringLiteral("no Parquet module is installed with this Mole (looked for %1)")
                .arg(candidateList.isEmpty() ? kFileName : candidateList.join(QStringLiteral(", "))));
    }

    // Kept once loaded (see keepForever): a reader the module made may live until
    // exit, and its code has to stay mapped for as long as it does. Every symbol is resolved now
    // rather than on first call, so a library that is too old fails here, with a
    // message, rather than later inside a preview.
    auto* library = new QLibrary(state.path);
    library->setLoadHints(QLibrary::ResolveAllSymbolsHint);
    if (!library->load()) {
        const QString message = library->errorString();
        delete library;
        return refuse(state, explainLoadFailure(message));
    }

    const auto abi = reinterpret_cast<MoleParquetModuleAbiFn>(library->resolve("mole_parquet_module_abi"));
    const auto qtVersion
        = reinterpret_cast<MoleParquetModuleQtVersionFn>(library->resolve("mole_parquet_module_qt_version"));
    const auto arrow
        = reinterpret_cast<MoleParquetModuleArrowFn>(library->resolve("mole_parquet_module_arrow"));
    const auto create
        = reinterpret_cast<MoleParquetModuleCreateFn>(library->resolve("mole_parquet_module_create"));
    if (!abi || !qtVersion || !arrow || !create) {
        library->unload();
        delete library;
        return refuse(state, QStringLiteral("%1 is not a Mole Parquet module").arg(state.path));
    }
    // Asked once and kept, because nothing in the module may be called after it is
    // unloaded -- including to say what it answered.
    const int moduleAbi = abi();
    if (moduleAbi != MOLE_PARQUET_MODULE_ABI) {
        library->unload();
        delete library;
        return refuse(state,
            QStringLiteral("it was built for a different Mole (module interface %1, this Mole speaks %2)")
                .arg(moduleAbi)
                .arg(MOLE_PARQUET_MODULE_ABI));
    }

    // **The check the loader cannot always make.** Qt tags every symbol with the
    // version that introduced it, so a module built against a newer Qt usually
    // fails above with `version Qt_6.10 not found`. Usually is not always: one that
    // happens to use nothing new would load, and then run inline code from newer
    // headers against an older library. Qt refuses a plugin built against a newer
    // Qt for the same reason; this is that rule, made here.
    const QVersionNumber running = QLibraryInfo::version();
    const int runningQt = QT_VERSION_CHECK(running.majorVersion(), running.minorVersion(), 0);
    const int builtQt = qtVersion() & 0xffff00;
    if (builtQt > runningQt) {
        library->unload();
        delete library;
        return refuse(state,
            QStringLiteral(
                "it was built for a newer system than this one (it needs Qt %1 and this system has %2)")
                .arg(qtVersionText(builtQt), qtVersionText(runningQt)));
    }

    state.loaded = true;
    state.arrowVersion = QString::fromLatin1(arrow());
    state.create = create;
    keepForever(library);
    return state;
}

const State& current()
{
    static const State state = [] {
        State loaded = loadFrom(candidates());
        // Once, in the session log, and never in a dialog: a missing module is the
        // normal state on two of the three distributions the .deb is for.
        if (loaded.loaded) {
            qInfo("Parquet module loaded from %s (Arrow %s)", qPrintable(loaded.path),
                qPrintable(loaded.arrowVersion));
        } else {
            qInfo("Parquet files open as file information: %s", qPrintable(loaded.reason));
        }
        return loaded;
    }();
    return state;
}

QString diagnosticLine()
{
    const State& state = current();
    if (state.loaded) {
        return QStringLiteral("parquet: module loaded, Arrow %1 (%2)").arg(state.arrowVersion, state.path);
    }
    return QStringLiteral("parquet: not available, so a .parquet file opens as file information -- %1")
        .arg(state.reason);
}

std::unique_ptr<ParquetReader> createReader(const QString& path)
{
    const State& state = current();
    if (!state.loaded || !state.create)
        return {};
    return std::unique_ptr<ParquetReader>(state.create(&path));
}

} // namespace mole::parquetModule
