#pragma once

#include <QJsonObject>
#include <QString>

namespace mole {

/// rclone, loaded into this process as a library.
///
/// Not the `rclone` binary and not a daemon: `librclone` is rclone's own C
/// interface, built from its source with Go and linked here. There is no child
/// process to supervise, no port to secure, and nothing for another program on
/// the machine to talk to.
///
/// Everything goes through one entry point -- `RcloneRPC(method, jsonInput)` --
/// which is rclone's remote-control API. That is a narrow, versioned surface,
/// and it is why one wrapper covers every backend rclone has.
///
/// Loaded lazily and by name, so a build without it still runs: the factory
/// reports itself unavailable and the interface leaves those drives out rather
/// than offering something that cannot work.
class RcloneLibrary
{
public:
    /// The one instance. rclone keeps global state -- a config, a token cache,
    /// a pool of connections -- and initialising it twice is not a thing it
    /// supports.
    static RcloneLibrary& instance();

    bool isAvailable();
    QString unavailableReason() const { return m_error; }
    /// Which rclone this is, once it has loaded.
    QString version();

    /// Calls an rclone remote-control method. `errorOut` receives rclone's own
    /// message, which is far more useful than anything this layer could invent.
    QJsonObject call(const QString& method, const QJsonObject& input, QString* errorOut = nullptr);

private:
    RcloneLibrary() = default;
    ~RcloneLibrary();
    RcloneLibrary(const RcloneLibrary&) = delete;
    RcloneLibrary& operator=(const RcloneLibrary&) = delete;

    bool load();

    void* m_handle = nullptr;
    void (*m_initialize)() = nullptr;
    void (*m_finalize)() = nullptr;
    void (*m_freeString)(char*) = nullptr;
    /// Returned by value: a struct of {char* Output; int Status;}.
    struct Result
    {
        char* output;
        int status;
    };
    Result (*m_rpc)(char*, char*) = nullptr;

    bool m_tried = false;
    bool m_ready = false;
    QString m_error;
    QString m_version;
};

} // namespace mole
