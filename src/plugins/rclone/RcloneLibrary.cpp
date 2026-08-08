#include "plugins/rclone/RcloneLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLibrary>

#include <dlfcn.h>

namespace mole {
namespace {

    /// Where to look, in order. The build tree first so a developer gets what they
    /// just built; then beside the executable, which is where the bundle puts it;
    /// then the ordinary library path.
    QStringList candidatePaths()
    {
        QStringList paths;

        const QByteArray override = qgetenv("MOLE_LIBRCLONE_PATH");
        if (!override.isEmpty())
            paths.append(QString::fromLocal8Bit(override));

        const QString applicationDirectory = QCoreApplication::applicationDirPath();
        if (!applicationDirectory.isEmpty()) {
            paths.append(QDir(applicationDirectory).filePath(QStringLiteral("librclone.so")));
            paths.append(QDir(applicationDirectory).filePath(QStringLiteral("../lib/librclone.so")));
        }

#ifdef MOLE_LIBRCLONE_BUILD_PATH
        paths.append(QStringLiteral(MOLE_LIBRCLONE_BUILD_PATH));
#endif

        paths.append(QStringLiteral("librclone.so"));
        paths.append(QStringLiteral("/usr/local/lib/librclone.so"));
        return paths;
    }

} // namespace

RcloneLibrary& RcloneLibrary::instance()
{
    static RcloneLibrary library;
    return library;
}

RcloneLibrary::~RcloneLibrary()
{
    // Deliberately not finalised and not unloaded. rclone's Go runtime has
    // background goroutines, and tearing it down while the process is exiting
    // has no benefit and several ways to crash.
}

bool RcloneLibrary::load()
{
    if (m_tried)
        return m_ready;
    m_tried = true;

    const QStringList paths = candidatePaths();
    for (const QString& path : paths) {
        // RTLD_GLOBAL, because the Go runtime inside expects its own symbols to
        // be resolvable; RTLD_NOW so a broken library fails here rather than at
        // the first call.
        m_handle = dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_GLOBAL);
        if (m_handle)
            break;
    }

    if (!m_handle) {
        m_error = QStringLiteral("librclone is not installed. Build it with `make librclone`, "
                                 "which needs the Go toolchain.");
        return false;
    }

    m_initialize = reinterpret_cast<void (*)()>(dlsym(m_handle, "RcloneInitialize"));
    m_finalize = reinterpret_cast<void (*)()>(dlsym(m_handle, "RcloneFinalize"));
    m_freeString = reinterpret_cast<void (*)(char*)>(dlsym(m_handle, "RcloneFreeString"));
    m_rpc = reinterpret_cast<Result (*)(char*, char*)>(dlsym(m_handle, "RcloneRPC"));

    if (!m_initialize || !m_rpc || !m_freeString) {
        m_error = QStringLiteral("This librclone does not export the interface expected");
        return false;
    }

    m_initialize();
    m_ready = true;
    return true;
}

bool RcloneLibrary::isAvailable()
{
    return load();
}

QString RcloneLibrary::version()
{
    if (!m_version.isEmpty())
        return m_version;
    if (!isAvailable())
        return {};

    const QJsonObject reply = call(QStringLiteral("core/version"), {});
    m_version = reply.value(QStringLiteral("version")).toString();
    return m_version;
}

QJsonObject RcloneLibrary::call(const QString& method, const QJsonObject& input, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return QJsonObject {};
    };

    if (!isAvailable())
        return fail(m_error);

    const QByteArray methodBytes = method.toUtf8();
    const QByteArray inputBytes = QJsonDocument(input).toJson(QJsonDocument::Compact);

    // The C API takes non-const pointers but does not modify them; Go copies
    // both into its own heap immediately.
    Result result
        = m_rpc(const_cast<char*>(methodBytes.constData()), const_cast<char*>(inputBytes.constData()));

    QByteArray output;
    if (result.output) {
        output = QByteArray(result.output);
        // Allocated by Go, so it has to go back to Go.
        m_freeString(result.output);
    }

    const QJsonDocument document = QJsonDocument::fromJson(output);
    const QJsonObject object = document.object();

    if (result.status < 200 || result.status >= 300) {
        // rclone's own message. Anything this layer invented instead would be
        // less specific and no more accurate.
        const QString message = object.value(QStringLiteral("error")).toString();
        return fail(
            message.isEmpty() ? QStringLiteral("rclone returned status %1").arg(result.status) : message);
    }

    if (errorOut)
        errorOut->clear();
    return object;
}

} // namespace mole
