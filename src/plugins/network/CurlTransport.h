#pragma once

#include "core/vfs/VfsTypes.h"

#include <QByteArray>
#include <QDateTime>
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
    /// Give up on a transfer that has moved nothing for this long. A wall-clock
    /// timeout cannot be used instead: a large file is slow for legitimate
    /// reasons, a dead connection is slow forever.
    ///
    /// Enforced by the loop in CurlPool::perform(), against the handle's own
    /// byte counters, so it means the same thing on every protocol. It is how
    /// patient one *connection* is; how long a whole transfer may go without
    /// progress is StreamingDownload's budget, and conflating the two is a
    /// mistake with its own paragraph in ADR-0013.
    long stallSeconds = 120;

    /// SFTP only. Empty means "wherever ssh would look".
    QString knownHostsPath;
    /// SFTP only: accept and remember a host we have never seen. A host whose
    /// key has *changed* is refused regardless -- that is the case worth
    /// refusing, and the one this flag does not cover.
    bool acceptUnknownHostKey = true;
};

/// Gives up on a transfer that has stopped moving.
///
/// libcurl has a guard of its own -- `CURLOPT_LOW_SPEED_LIMIT` with
/// `CURLOPT_LOW_SPEED_TIME` -- and **for SFTP it does not fire**. Measured
/// against a server whose link was cut in the middle of a transfer: the progress
/// callback went on being called twice a second with the byte count frozen, and
/// the transfer was still running long after the guard's time had passed. So the
/// SSH layer is not blocking libcurl's loop, which was the standing guess; the
/// low-speed check simply never reaches a verdict on that protocol.
///
/// It is applied here instead, in the progress callback, which is called
/// throughout -- that is the whole reason this works. A transfer that neither
/// finishes nor fails is the one outcome a file manager may not produce: a job
/// that is going to fail must fail while somebody is still watching.
///
/// **This ends a connection, not a transfer.** Aborting from the callback does
/// not even make `curl_easy_perform` return while the thread is inside the SSH
/// layer waiting on a socket the kernel is still retransmitting into;
/// `TCP_USER_TIMEOUT`, set to its own short figure, is what ends that. What ends
/// the *transfer* is StreamingDownload's budget, which is reset by every byte
/// that arrives and does not care how many connections it took.
///
/// Movement rather than speed, deliberately. libcurl's guard asks "is it slower
/// than N bytes a second", which needs a rate and a window and gets both wrong
/// on a link that is legitimately slow. This asks "has anything at all arrived",
/// which a transfer over any working connection answers yes to, and a dead one
/// never does.
class StallWatch
{
public:
    /// Zero or less turns it off, which is what a caller that wants libcurl's
    /// guard and nothing else passes.
    explicit StallWatch(qint64 stallSeconds)
        : m_stallMs(stallSeconds > 0 ? stallSeconds * 1000 : -1)
    {
    }

    /// `movedBytes` is everything this transfer has carried so far and `nowMs`
    /// is how long it has been going. True once nothing has moved for long
    /// enough that it is not going to finish.
    ///
    /// The clock is a parameter rather than read here, so that what decides this
    /// is the same thing a test can drive.
    bool hasStalled(qint64 movedBytes, qint64 nowMs)
    {
        if (movedBytes != m_lastMoved) {
            m_lastMoved = movedBytes;
            m_lastMovedMs = nowMs;
            return false;
        }
        return m_stallMs > 0 && nowMs - m_lastMovedMs >= m_stallMs;
    }

    /// How long it waits, in milliseconds; -1 when it is off.
    qint64 patienceMs() const { return m_stallMs; }

private:
    qint64 m_stallMs = -1;
    qint64 m_lastMoved = -1;
    qint64 m_lastMovedMs = 0;
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

    /// How long the payload was said to be and how much of it arrived. Both are
    /// filled in only for a download into a device -- see CurlPool::perform --
    /// and `expectedBytes` stays -1 when the other end never said. A transfer
    /// that ends short of a length the server itself announced is a truncated
    /// file, whatever the protocol thought of it.
    qint64 expectedBytes = -1;
    qint64 receivedBytes = -1;

    /// The URL the transfer actually finished on, which is not the one it
    /// started on whenever a redirect was followed. A WebDAV server answers a
    /// GET on a collection by redirecting to the same path with a slash on the
    /// end, so this is how a caller finds out it asked for a directory without
    /// paying for a PROPFIND to ask.
    QByteArray effectiveUrl;

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
        /// `pooled` false means the handle is closed when the lease ends
        /// rather than handed to the next caller.
        Lease(CurlPool* pool, CURL* handle, bool pooled = true);
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        CURL* get() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        /// Points the request at an address, and remembers it. Every backend
        /// goes through here rather than setting CURLOPT_URL itself, because a
        /// log line about a transfer that cannot say which file it was is not
        /// worth writing -- and curl's own idea of the "effective URL" is the
        /// previous transfer's on a handle that has been reused.
        void setUrl(const QByteArray& url);
        const QByteArray& url() const { return m_url; }

        /// Gives this one transfer a connection nobody else has touched, and
        /// closes it afterwards rather than returning it to the cache.
        ///
        /// The pool exists to avoid exactly this, so it is asked for only where
        /// reuse is known to break: a large SFTP payload over a connection that
        /// has already carried an operation stops dead partway through. See
        /// SftpFileSystem for what was measured.
        void useOwnConnection();

        /// Says this handle must be closed rather than pooled, because its
        /// transfer was abandoned part way and its connection is mid-message.
        /// The next caller inheriting that would read somebody else's reply.
        ///
        /// Called by perform() when its loop ends on a stall or a cancellation
        /// rather than on the transfer finishing.
        void abandon() const;

    private:
        CurlPool* m_pool = nullptr;
        CURL* m_handle = nullptr;
        QByteArray m_url;
        /// Mutable because abandoning is a fact discovered during the transfer,
        /// and perform() is handed the lease by const reference.
        mutable bool m_pooled = true;
    };

    /// Enough to keep every TaskManager worker warm. Past that a handle is
    /// cheaper to close than to hold a connection nobody is using.
    static constexpr size_t kMaxIdleHandles = 8;

    explicit CurlPool(TransportOptions options);
    ~CurlPool();

    const TransportOptions& options() const { return m_options; }

    /// Never fails to a null lease unless libcurl itself cannot allocate.
    Lease take();

    /// A handle nobody has used, thrown away rather than pooled when the lease
    /// ends. For transfers that must not inherit anything from an earlier one.
    Lease takeFresh();

    /// Runs the request on a leased handle and returns when it is done.
    /// `cancel` is polled throughout, so a slow transfer stops promptly.
    ///
    /// With a `sink` the payload is written straight to it and Response::body is
    /// left empty -- which is how a multi-gigabyte download avoids becoming a
    /// multi-gigabyte QByteArray. Without one the body is collected in memory,
    /// which is what every listing and XML answer wants.
    /// Runs one transfer, in a loop this class owns.
    ///
    /// Through libcurl's multi interface rather than `curl_easy_perform`, and not
    /// for speed or concurrency -- `curl_easy_perform` is itself a wrapper over
    /// the same machinery. It is so that the decision to stop is Mole's. A loop
    /// somebody else owns has to be asked to stop and may decline: on SFTP the
    /// progress callback was consulted twice a second throughout a dead link and
    /// returning "stop" from it did not end the call, so what actually ended
    /// transfers was a kernel socket timeout. See ADR-0049.
    ///
    /// Two things follow, on every protocol. A cancellation takes effect at the
    /// next poll. And "nothing has arrived for stallSeconds" ends the transfer at
    /// stallSeconds, whatever the protocol is doing and whatever the socket is
    /// doing.
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

/// Reads a date the way HTTP writes one, and the two other ways servers do.
///
/// One function rather than one per protocol, because the awkward part is the
/// same everywhere and getting it wrong looks like a server that keeps no
/// timestamps: **Qt's RFC 2822 reader accepts a numeric offset and nothing
/// else**, while every HTTP date ends in "GMT" -- so the bare parse returns an
/// invalid QDateTime for `Wed, 12 Oct 2022 10:00:00 GMT`, which is the
/// `Last-Modified` of every object in every bucket. WebDAV had worked around it
/// in its own listing parser for months while S3's stat() had not, which is
/// what made this shared. See MOLE-347.
QDateTime httpDate(const QString& text);

} // namespace mole::net
