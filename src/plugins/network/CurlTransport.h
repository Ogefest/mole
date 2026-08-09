#pragma once

#include "core/vfs/VfsTypes.h"

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QPair>
#include <QString>

#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <vector>

namespace mole::net {

/// Header name/value pairs, as both the transport and the signer deal in.
///
/// Declared identically in S3Signer.h on purpose: repeating a type alias is
/// legal, and it lets the signer stay a pure algorithm that does not pull in
/// curl's headers just to name a list of strings.
using HeaderList = QList<QPair<QByteArray, QByteArray>>;

/// Calls curl_global_init exactly once, whichever backend gets there first.
void ensureCurlInitialised();

/// Everything a backend's transfers have in common: who we are, how patient to
/// be, and how strict to be about the other end's identity.
struct TransportOptions
{
    QString username;
    QString password;

    /// Verify the server's TLS certificate. Off is offered for the one case that
    /// actually needs it -- a MinIO or Ceph install behind a self-signed cert --
    /// and is never the default.
    bool verifyTls = true;

    long connectTimeoutSeconds = 20;
    /// Give up when a transfer moves fewer than this many bytes per second for
    /// `stallSeconds`. A wall-clock timeout cannot be used instead: a large file
    /// is slow for legitimate reasons, a dead connection is slow forever.
    long stallBytesPerSecond = 1;
    long stallSeconds = 120;

    /// SFTP only. Empty means "wherever ssh would look".
    QString knownHostsPath;
    /// SFTP only: accept and remember a host we have never seen. A host whose
    /// key has *changed* is refused regardless -- that is the case worth
    /// refusing, and the one this flag does not cover.
    bool acceptUnknownHostKey = true;
};

/// One completed transfer.
struct Response
{
    CURLcode code = CURLE_OK;
    /// HTTP status, or 0 for protocols that have none.
    long status = 0;
    QByteArray body;
    HeaderList headers;
    /// curl's own error buffer, which says far more than the code alone.
    QString detail;

    /// Case-insensitive lookup; empty when absent.
    QByteArray header(const char* name) const;
    bool httpOk() const { return status >= 200 && status < 300; }
};

/// What a request should send, when it sends anything.
struct RequestBody
{
    QByteArray data;
    bool present = false;

    static RequestBody none() { return {}; }
    static RequestBody of(QByteArray bytes) { return { std::move(bytes), true }; }
};

/// A pool of curl easy handles, one lent out per call.
///
/// Two things force this shape. A curl easy handle is not thread-safe, and
/// IFileSystem is documented as tolerating concurrent calls from several worker
/// threads -- so a handle cannot simply be a member. But a fresh handle each
/// time would throw away libcurl's connection cache, and an SFTP drive that
/// renegotiates SSH for every listing is unusable. Lending handles out keeps
/// both: exclusive use while borrowed, and a warm connection to come back to.
class CurlPool
{
public:
    /// Borrowed handle, returned to the pool when it goes out of scope.
    class Lease
    {
    public:
        Lease(CurlPool* pool, CURL* handle);
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        CURL* get() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

    private:
        CurlPool* m_pool = nullptr;
        CURL* m_handle = nullptr;
    };

    /// Enough to keep every TaskManager worker warm. Past that a handle is
    /// cheaper to close than to hold a connection nobody is using.
    static constexpr size_t kMaxIdleHandles = 8;

    explicit CurlPool(TransportOptions options);
    ~CurlPool();

    const TransportOptions& options() const { return m_options; }

    /// Never fails to a null lease unless libcurl itself cannot allocate.
    Lease take();

    /// Runs the request on a leased handle and returns when it is done.
    /// `cancel` is polled throughout, so a slow transfer stops promptly.
    ///
    /// With a `sink` the payload is written straight to it and Response::body is
    /// left empty -- which is how a multi-gigabyte download avoids becoming a
    /// multi-gigabyte QByteArray. Without one the body is collected in memory,
    /// which is what every listing and XML answer wants.
    Response perform(const Lease& lease, const CancelToken& cancel, QIODevice* sink = nullptr);

    /// Points a request at `payload` as the thing to upload. The device must
    /// stay alive and positioned until perform() returns.
    static void sendFrom(const Lease& lease, QIODevice& payload, qint64 size);

private:
    friend class Lease;
    void give(CURL* handle);
    /// Applies the shared options and clears per-request state.
    void prepare(CURL* handle) const;

    TransportOptions m_options;
    std::mutex m_mutex;
    std::vector<CURL*> m_idle;
};

/// How to read Response::status for a given protocol.
enum class StatusMeaning {
    /// It is an HTTP status, and it decides whether the request succeeded. Used
    /// by S3 and WebDAV.
    Http,
    /// The protocol reports success through the transfer result alone, and
    /// `status` is merely the last reply code it happened to send. FTP finishes a
    /// perfectly good listing with 226 "Transfer complete", which read as an HTTP
    /// status would be a failure -- so for these protocols the code is what
    /// counts and the status is ignored.
    ProtocolReply
};

/// Maps a finished transfer onto the VFS error vocabulary.
VfsError errorFor(const Response& response, const QString& what, StatusMeaning meaning = StatusMeaning::Http);

/// True when the transfer failed only because the caller cancelled it.
bool wasCancelled(const Response& response);

/// Percent-encodes one path segment, leaving nothing for a server to
/// misinterpret. Used to build request paths out of user-supplied names.
QByteArray encodeSegment(const QString& segment);

/// Encodes a whole '/'-separated path, segment by segment, keeping the slashes.
QByteArray encodePath(const QString& path);

} // namespace mole::net
