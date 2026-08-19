#include "plugins/network/CurlTransport.h"

#include "core/diagnostics/Diagnostics.h"

#include <QElapsedTimer>
#include <QUrl>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace mole::net {
namespace {

    std::once_flag g_initOnce;

    /// Numbers the transfers so interleaved lines from several worker threads
    /// can be told apart. Only ever read by the log.
    std::atomic<quint64> g_transferCounter { 0 };

    /// What the trace callback needs to know about the transfer it is narrating.
    struct Trace
    {
        quint64 id = 0;
    };

    /// A header that would put a credential in the log, replaced by its name.
    /// The session log lives on disk and gets sent to whoever is helping; a
    /// signature or a Basic password has no business travelling with it.
    QByteArray withoutSecrets(const QByteArray& line)
    {
        const int colon = line.indexOf(':');
        if (colon <= 0)
            return line;
        const QByteArray name = line.left(colon).toLower();
        if (name == "authorization" || name == "proxy-authorization" || name == "x-amz-security-token")
            return line.left(colon) + ": <redacted>";
        return line;
    }

    /// libcurl's own commentary, forwarded into the log rather than to stderr.
    int traceCurl(CURL*, curl_infotype type, char* data, size_t size, void* userData)
    {
        const auto* trace = static_cast<const Trace*>(userData);

        const char* marker = nullptr;
        switch (type) {
        case CURLINFO_TEXT:
            marker = "*";
            break;
        case CURLINFO_HEADER_IN:
            marker = "<";
            break;
        case CURLINFO_HEADER_OUT:
            marker = ">";
            break;
        default:
            // The payload itself. How much of it arrived is in the line perform()
            // writes at the end; repeating it for every block would bury the
            // state changes that are the reason to read a trace at all.
            return 0;
        }

        const QByteArray text = QByteArray(data, static_cast<int>(size));
        for (const QByteArray& line : text.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty())
                continue;
            qCDebug(curlLog, "#%llu %s %s", static_cast<unsigned long long>(trace->id), marker,
                withoutSecrets(trimmed).constData());
        }
        return 0;
    }

    size_t writeToBuffer(char* data, size_t size, size_t count, void* userData)
    {
        auto* out = static_cast<QByteArray*>(userData);
        const size_t bytes = size * count;
        out->append(data, static_cast<int>(bytes));
        return bytes;
    }

    size_t writeToDevice(char* data, size_t size, size_t count, void* userData)
    {
        auto* sink = static_cast<QIODevice*>(userData);
        const qint64 bytes = static_cast<qint64>(size * count);
        // A short write is how libcurl is told to stop: returning anything other
        // than the byte count aborts the transfer with CURLE_WRITE_ERROR, which
        // is exactly right when the local disk filled up.
        return sink->write(data, bytes) == bytes ? static_cast<size_t>(bytes) : 0;
    }

    size_t readFromDevice(char* buffer, size_t size, size_t count, void* userData)
    {
        auto* source = static_cast<QIODevice*>(userData);
        const qint64 read = source->read(buffer, static_cast<qint64>(size * count));
        return read < 0 ? CURL_READFUNC_ABORT : static_cast<size_t>(read);
    }

    /// Puts the payload back to a byte offset so curl can send it again.
    ///
    /// curl needs this far more often than it sounds. An authenticating server
    /// answers the first request with a 401, and curl retries -- which means
    /// sending the body a second time. Without a way to rewind, that retry is
    /// impossible and the transfer dies with "necessary data rewind wasn't
    /// possible", whatever the request was and however small.
    ///
    /// A staged upload is a temporary file and can always oblige. A stream being
    /// written as it is sent cannot: the bytes it already handed over are gone.
    /// Saying so is the point -- curl asks a server's permission before sending
    /// a body it cannot repeat, which is what Expect: 100-continue is for.
    int seekDevice(void* userData, curl_off_t offset, int origin)
    {
        auto* source = static_cast<QIODevice*>(userData);
        if (origin != SEEK_SET || source->isSequential())
            return CURL_SEEKFUNC_CANTSEEK;
        return source->seek(offset) ? CURL_SEEKFUNC_OK : CURL_SEEKFUNC_FAIL;
    }

    size_t collectHeader(char* data, size_t size, size_t count, void* userData)
    {
        auto* out = static_cast<QList<QPair<QByteArray, QByteArray>>*>(userData);
        const size_t bytes = size * count;
        const QByteArray line = QByteArray(data, static_cast<int>(bytes)).trimmed();
        const int colon = line.indexOf(':');
        if (colon > 0)
            out->append({ line.left(colon).toLower(), line.mid(colon + 1).trimmed() });
        return bytes;
    }

    /// How long the kernel may go on retransmitting into a link that has gone
    /// away before the socket is failed, in seconds. A backstop: the transfer is
    /// over long before this, by the guard's decision.
    constexpr long kSocketPatienceSeconds = 20;

    /// How long one turn of the transfer loop waits for something to happen.
    /// Short enough that a cancellation is felt at once and a stall is noticed
    /// within a poll of the figure the guard was given.
    constexpr int kPollMs = 200;

    /// Bounds how long the kernel will go on retransmitting into a link that has
    /// gone away, on the socket libcurl has just opened.
    ///
    /// The other half of the fix StallWatch is the first half of, and the half
    /// that was not obvious. Aborting from the progress callback is not enough
    /// on its own: measured against a server whose link was cut, the callback
    /// fired on time and returned "stop", and `curl_easy_perform` still did not
    /// return -- the thread was inside the SSH layer, waiting on a socket that
    /// the kernel was still faithfully retransmitting into.
    ///
    /// TCP keepalive does not cover it. Keepalive only runs on an *idle*
    /// connection, and a transfer cut off mid-flight has unacknowledged data, so
    /// the kernel is in retransmission instead -- which is bounded by
    /// `tcp_retries2`, about fifteen minutes by default. That is the 641 seconds
    /// the fault was reported with.
    ///
    /// `TCP_USER_TIMEOUT` is the one that applies: it caps how long *sent* data
    /// may go unacknowledged before the socket is failed. Linux only; elsewhere
    /// the guard still fires and how long the socket takes to notice is the
    /// platform's business.
    ///
    /// **A backstop, and nothing decides on it any more.** It was briefly the
    /// thing that ended a stalled transfer, because the guard could only ask
    /// libcurl to stop and libcurl went on waiting. perform() owns the loop now,
    /// so the guard ends the transfer itself and this is only here to stop a
    /// socket the kernel is retransmitting into from being held open for the
    /// fifteen minutes `tcp_retries2` allows -- which matters for the file
    /// descriptor, not for the answer anybody gets.
    int boundTheSocketsPatience(void*, curl_socket_t handle, curlsocktype purpose)
    {
        if (purpose != CURLSOCKTYPE_IPCXN)
            return CURL_SOCKOPT_OK;
#ifdef TCP_USER_TIMEOUT
        if (kSocketPatienceSeconds > 0) {
            const unsigned int ms = static_cast<unsigned int>(kSocketPatienceSeconds) * 1000;
            setsockopt(handle, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, sizeof(ms));
        }
#endif
        return CURL_SOCKOPT_OK;
    }

    /// What the progress callback needs for its two jobs.
    struct ProgressWatch
    {
        const CancelToken* cancel = nullptr;
        StallWatch stall { 0 };
        QElapsedTimer since;
        /// Set when this callback is what stopped the transfer because nothing
        /// was moving. libcurl reports a cancellation and a stall the same way --
        /// CURLE_ABORTED_BY_CALLBACK -- and they are not the same thing to
        /// report to somebody.
        bool stalled = false;
    };

    /// Polled by libcurl during the transfer; a non-zero return aborts it. This
    /// is what turns a CancelToken into a transfer that actually stops, rather
    /// than one that finishes and is then thrown away -- and, since libcurl's own
    /// low-speed guard does not fire for SFTP, it is also where a transfer that
    /// has stopped moving is given up on. See StallWatch.
    int reportProgress(void* userData, curl_off_t, curl_off_t received, curl_off_t, curl_off_t sent)
    {
        auto* watch = static_cast<ProgressWatch*>(userData);
        if (watch->cancel->isCancelled())
            return 1;
        if (watch->stall.hasStalled(
                static_cast<qint64>(received) + static_cast<qint64>(sent), watch->since.elapsed())) {
            watch->stalled = true;
            return 1;
        }
        return 0;
    }

    /// Host key policy, stated once: a host we have not met is accepted and
    /// remembered, a host whose key has changed is refused. Only the second case
    /// is evidence of anything, and it is the one that must not be waved through.
    int checkHostKey(CURL*, const curl_khkey*, const curl_khkey*, curl_khmatch match, void* userData)
    {
        const auto* options = static_cast<const TransportOptions*>(userData);
        switch (match) {
        case CURLKHMATCH_OK:
            return CURLKHSTAT_FINE;
        case CURLKHMATCH_MISSING:
            return options->acceptUnknownHostKey ? CURLKHSTAT_FINE_ADD_TO_FILE : CURLKHSTAT_REJECT;
        case CURLKHMATCH_MISMATCH:
        default:
            return CURLKHSTAT_REJECT;
        }
    }

} // namespace

void ensureCurlInitialised()
{
    std::call_once(g_initOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

QByteArray Response::header(const char* name) const
{
    const QByteArray wanted = QByteArray(name).toLower();
    for (const auto& entry : headers) {
        if (entry.first == wanted)
            return entry.second;
    }
    return {};
}

// ---- Lease -----------------------------------------------------------------

CurlPool::Lease::Lease(CurlPool* pool, CURL* handle, bool pooled)
    : m_pool(pool)
    , m_handle(handle)
    , m_pooled(pooled)
{
}

CurlPool::Lease::~Lease()
{
    if (!m_handle)
        return;
    if (m_pool && m_pooled)
        m_pool->give(m_handle);
    else
        curl_easy_cleanup(m_handle);
}

CurlPool::Lease::Lease(Lease&& other) noexcept
    : m_pool(other.m_pool)
    , m_handle(other.m_handle)
    , m_url(std::move(other.m_url))
    , m_pooled(other.m_pooled)
{
    other.m_pool = nullptr;
    other.m_handle = nullptr;
}

CurlPool::Lease& CurlPool::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        if (m_pool && m_handle)
            m_pool->give(m_handle);
        m_pool = other.m_pool;
        m_handle = other.m_handle;
        m_url = std::move(other.m_url);
        m_pooled = other.m_pooled;
        other.m_pool = nullptr;
        other.m_handle = nullptr;
    }
    return *this;
}

void CurlPool::Lease::setUrl(const QByteArray& url)
{
    m_url = url;
    if (m_handle)
        curl_easy_setopt(m_handle, CURLOPT_URL, m_url.constData());
}

void CurlPool::Lease::abandon() const
{
    if (!m_handle)
        return;
    m_pooled = false;
    // Belt and braces: even if something else pools it, the connection is not
    // reused for another request.
    curl_easy_setopt(m_handle, CURLOPT_FORBID_REUSE, 1L);
}

void CurlPool::Lease::useOwnConnection()
{
    if (!m_handle)
        return;
    // Both halves matter. The first says "do not pick up the warm connection
    // this handle is holding"; the second says "and do not leave this one behind
    // for the next caller either", because a connection that has carried a large
    // transfer is exactly what the next one must not inherit.
    curl_easy_setopt(m_handle, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(m_handle, CURLOPT_FORBID_REUSE, 1L);
}

// ---- CurlPool --------------------------------------------------------------

CurlPool::CurlPool(TransportOptions options)
    : m_options(std::move(options))
{
    ensureCurlInitialised();
}

CurlPool::~CurlPool()
{
    for (CURL* handle : m_idle)
        curl_easy_cleanup(handle);
}

CurlPool::Lease CurlPool::take()
{
    CURL* handle = nullptr;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_idle.empty()) {
            handle = m_idle.back();
            m_idle.pop_back();
        }
    }
    if (!handle)
        handle = curl_easy_init();
    if (handle)
        prepare(handle);
    return Lease(this, handle);
}

CurlPool::Lease CurlPool::takeFresh()
{
    CURL* handle = curl_easy_init();
    if (handle)
        prepare(handle);
    return Lease(this, handle, false);
}

void CurlPool::give(CURL* handle)
{
    // Only the transfer state is reset; the connection cache lives on the
    // handle, which is the whole reason for keeping it.
    const std::lock_guard<std::mutex> guard(m_mutex);
    if (m_idle.size() >= kMaxIdleHandles) {
        curl_easy_cleanup(handle);
        return;
    }
    m_idle.push_back(handle);
}

void CurlPool::prepare(CURL* handle) const
{
    curl_easy_reset(handle);

    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, m_options.connectTimeoutSeconds);
    // No CURLOPT_LOW_SPEED_LIMIT/TIME. It was libcurl's own attempt at this
    // guard and it never reached a verdict on SFTP at all; now that perform()
    // owns the loop, the guard is consulted there against the handle's counters
    // and a second one underneath it could only disagree.
    curl_easy_setopt(handle, CURLOPT_SOCKOPTFUNCTION, boundTheSocketsPatience);
    curl_easy_setopt(handle, CURLOPT_SOCKOPTDATA, const_cast<TransportOptions*>(&m_options));
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "Mole/0.1");

    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, m_options.verifyTls ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, m_options.verifyTls ? 2L : 0L);

    if (!m_options.username.isEmpty()) {
        curl_easy_setopt(handle, CURLOPT_USERNAME, m_options.username.toUtf8().constData());
        curl_easy_setopt(handle, CURLOPT_PASSWORD, m_options.password.toUtf8().constData());
    }

    if (!m_options.knownHostsPath.isEmpty()) {
        // Not every libcurl build can do this -- it depends which SSH library
        // curl was compiled against -- so a refusal here is not fatal. What
        // must not happen is silently continuing with no verification at all,
        // which is why the fallback disables SFTP rather than the checking.
        curl_easy_setopt(handle, CURLOPT_SSH_KNOWNHOSTS, m_options.knownHostsPath.toUtf8().constData());
        curl_easy_setopt(handle, CURLOPT_SSH_KEYFUNCTION, checkHostKey);
        curl_easy_setopt(handle, CURLOPT_SSH_KEYDATA, &m_options);
    }
}

void CurlPool::sendFrom(const Lease& lease, QIODevice& payload, qint64 size)
{
    CURL* handle = lease.get();
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, readFromDevice);
    curl_easy_setopt(handle, CURLOPT_READDATA, &payload);
    curl_easy_setopt(handle, CURLOPT_SEEKFUNCTION, seekDevice);
    curl_easy_setopt(handle, CURLOPT_SEEKDATA, &payload);
    // A request carrying a file does not follow redirects.
    //
    // Handles are set up with FOLLOWLOCATION on, which is right for a listing
    // and wrong for this: a server answering a PUT with a redirect would have
    // the file sent to whatever address it named. That was harmless only for as
    // long as the body could not be replayed at all -- an accident rather than a
    // policy, and the moment a staged upload became rewindable the file started
    // going wherever it was pointed.
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    // A negative size means the length is not known in advance, which a stream
    // being written as it is sent cannot know. Protocols that need it up front
    // do not use this path -- see StreamingUpload.
    if (size >= 0)
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
}

Response CurlPool::perform(const Lease& lease, const CancelToken& cancel, QIODevice* sink)
{
    Response response;
    if (!lease) {
        response.code = CURLE_FAILED_INIT;
        response.detail = QStringLiteral("could not allocate a transfer handle");
        return response;
    }
    // Checked before any I/O, so an already-cancelled operation costs nothing
    // and reports Cancelled rather than whatever the network happened to say.
    if (cancel.isCancelled()) {
        response.code = CURLE_ABORTED_BY_CALLBACK;
        return response;
    }

    CURL* handle = lease.get();
    char errorBuffer[CURL_ERROR_SIZE] = { 0 };

    const quint64 id = ++g_transferCounter;
    Trace trace { id };
    if (curlLog().isDebugEnabled()) {
        curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(handle, CURLOPT_DEBUGFUNCTION, traceCurl);
        curl_easy_setopt(handle, CURLOPT_DEBUGDATA, &trace);
    }

    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
    if (sink) {
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToDevice);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, sink);
    } else {
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToBuffer);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    }
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, collectHeader);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    // The callback stays for what it is good at, which is being called often: a
    // cancellation noticed there takes effect before the next poll comes round.
    // It is no longer the only way out, and it no longer decides anything about
    // stalling -- the loop below does that, against the handle's own counters.
    ProgressWatch watch { &cancel, StallWatch(0), {}, false };
    watch.since.start();
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, reportProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &watch);

    qCDebug(networkLog, "#%llu start %s%s", static_cast<unsigned long long>(id),
        lease.url().isEmpty() ? "(no address)" : lease.url().constData(), sink ? " -> file" : "");

    QElapsedTimer clock;
    clock.start();

    // ---- the loop this class owns --------------------------------------
    //
    // curl_easy_perform would block here until libcurl decided the transfer was
    // over, which made every guard Mole has an attempt to influence somebody
    // else's loop. Driving it here costs a multi handle per transfer and buys
    // the two decisions that matter: when to stop waiting, and on what grounds.
    CURLM* multi = curl_multi_init();
    if (!multi) {
        response.code = CURLE_FAILED_INIT;
        response.detail = QStringLiteral("could not start a transfer loop");
        return response;
    }
    curl_multi_add_handle(multi, handle);

    StallWatch stall(m_options.stallSeconds);
    bool cancelled = false;
    bool stalled = false;
    int running = 1;

    while (running > 0) {
        const CURLMcode stepped = curl_multi_perform(multi, &running);
        if (stepped != CURLM_OK) {
            response.detail = QString::fromUtf8(curl_multi_strerror(stepped));
            break;
        }
        if (running == 0)
            break;

        // A few hundred milliseconds, so cancelling is felt at once and a
        // stalled transfer is noticed within a poll of the guard's figure.
        int ready = 0;
        curl_multi_poll(multi, nullptr, 0, kPollMs, &ready);

        if (cancel.isCancelled()) {
            cancelled = true;
            break;
        }

        // From the handle's own counters rather than from what the callback last
        // happened to be told. Both directions, because an upload stalls the
        // same way a download does and neither is this loop's business to know.
        curl_off_t received = 0;
        curl_off_t sent = 0;
        curl_easy_getinfo(handle, CURLINFO_SIZE_DOWNLOAD_T, &received);
        curl_easy_getinfo(handle, CURLINFO_SIZE_UPLOAD_T, &sent);
        if (stall.hasStalled(static_cast<qint64>(received) + static_cast<qint64>(sent), clock.elapsed())) {
            stalled = true;
            break;
        }
    }

    // The result belongs to the message rather than to a return value now.
    int leftToRead = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &leftToRead)) {
        if (message->msg == CURLMSG_DONE && message->easy_handle == handle)
            response.code = message->data.result;
    }

    curl_multi_remove_handle(multi, handle);
    curl_multi_cleanup(multi);

    if (cancelled || stalled) {
        // A transfer stopped part way leaves its connection mid-message, and the
        // next caller must not inherit it.
        lease.abandon();
    }
    if (cancelled) {
        response.code = CURLE_ABORTED_BY_CALLBACK;
    } else if (stalled) {
        response.code = CURLE_OPERATION_TIMEDOUT;
        response.detail = QStringLiteral("nothing arrived for %1 seconds, so it was given up on")
                              .arg(stall.patienceMs() / 1000);
    }

    const qint64 elapsed = clock.elapsed();

    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    const char* landedOn = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &landedOn) == CURLE_OK && landedOn)
        response.effectiveUrl = landedOn;

    // Detached before the handle goes back to the pool: the buffer is on our
    // stack and must not outlive this call.
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, nullptr);
    curl_easy_setopt(handle, CURLOPT_DEBUGDATA, nullptr);

    curl_off_t received = 0;
    curl_off_t announced = -1;
    curl_off_t sent = 0;
    long connects = 0;
    curl_easy_getinfo(handle, CURLINFO_SIZE_DOWNLOAD_T, &received);
    curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &announced);
    curl_easy_getinfo(handle, CURLINFO_SIZE_UPLOAD_T, &sent);
    curl_easy_getinfo(handle, CURLINFO_NUM_CONNECTS, &connects);

    if (sink) {
        // Only for a download into a device, which is the one case where both
        // numbers mean the same thing. A HEAD -- runCommand's NOBODY, S3's
        // stat -- is told the length and asks for none of it, and comparing the
        // two there would call every one of them truncated.
        response.expectedBytes = announced;
        response.receivedBytes = received;
    }

    // Only when nothing better has been said. The loop above knows things
    // libcurl's own string does not -- that this was a stall and how long it
    // waited -- and "Timeout was reached" is the version of that nobody can act
    // on.
    if (response.code != CURLE_OK && response.detail.isEmpty()) {
        response.detail = errorBuffer[0] != 0 ? QString::fromUtf8(errorBuffer)
                                              : QString::fromUtf8(curl_easy_strerror(response.code));
    }

    qCDebug(networkLog, "#%llu %s in %lld ms: %lld of %lld bytes down, %lld up, status %ld, %s%s%s",
        static_cast<unsigned long long>(id), response.code == CURLE_OK ? "done" : "failed", elapsed,
        static_cast<long long>(received), static_cast<long long>(announced), static_cast<long long>(sent),
        response.status, connects > 0 ? "new connection" : "connection reused",
        response.code == CURLE_OK ? "" : ", ", qPrintable(response.detail));

    // Gated on `sink` for the same reason the error is: a HEAD is told the length
    // and asks for none of it, so without this every stat on an object store
    // announces a truncated download that never happened.
    if (sink && response.code == CURLE_OK && announced > 0 && received < announced) {
        qCWarning(networkLog, "#%llu %s ended after %lld of the %lld bytes it was promised",
            static_cast<unsigned long long>(id), lease.url().constData(), static_cast<long long>(received),
            static_cast<long long>(announced));
    }
    return response;
}

// ---- error mapping ---------------------------------------------------------

bool wasCancelled(const Response& response)
{
    return response.code == CURLE_ABORTED_BY_CALLBACK;
}

VfsError errorFor(const Response& response, const QString& what, StatusMeaning meaning)
{
    if (wasCancelled(response))
        return VfsError::make(VfsError::Cancelled, QStringLiteral("%1 was cancelled").arg(what));

    const QString detail = response.detail.isEmpty() ? QStringLiteral("no further detail") : response.detail;
    const auto fail = [&](VfsError::Code code, const QString& text) {
        return VfsError::make(code, QStringLiteral("%1: %2").arg(what, text));
    };

    switch (response.code) {
    case CURLE_OK:
        break;
    case CURLE_REMOTE_FILE_NOT_FOUND:
    case CURLE_TFTP_NOTFOUND:
        return fail(VfsError::NotFound, QStringLiteral("no such file or directory"));
    case CURLE_REMOTE_ACCESS_DENIED:
    case CURLE_LOGIN_DENIED:
    case CURLE_AUTH_ERROR:
        return fail(VfsError::AccessDenied, detail);
    case CURLE_REMOTE_FILE_EXISTS:
        return fail(VfsError::AlreadyExists, detail);
    case CURLE_REMOTE_DISK_FULL:
    case CURLE_WRITE_ERROR:
    case CURLE_READ_ERROR:
    case CURLE_UPLOAD_FAILED:
    // Never reported by a transfer that ran: it is how a backend says the
    // transfer could not be started at all, which is a local failure and not
    // something the other end did.
    case CURLE_FAILED_INIT:
        return fail(VfsError::IoError, detail);
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_NOT_BUILT_IN:
        return fail(VfsError::NotSupported, detail);
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
        return fail(VfsError::NetworkError, detail);
    default:
        return fail(VfsError::Unknown, detail);
    }

    // A transfer curl called successful can still be short. libssh2 reports a
    // dropped SFTP channel as a clean end of file, so curl believes the download
    // finished and hands back whatever arrived; the same is true of an HTTP
    // response cut off after its Content-Length was read. Both look exactly like
    // a complete file to everything above, which is the worst possible outcome
    // of a copy -- so the one number that disproves it is checked here, for
    // every protocol, before anybody is told the file is good.
    if (response.expectedBytes > 0 && response.receivedBytes >= 0
        && response.receivedBytes < response.expectedBytes) {
        return fail(VfsError::IoError,
            QStringLiteral("the transfer stopped after %1 of %2 bytes")
                .arg(response.receivedBytes)
                .arg(response.expectedBytes));
    }

    // The transfer itself worked. For SFTP and FTP that is the whole answer.
    if (meaning == StatusMeaning::ProtocolReply)
        return VfsError::ok();

    // For HTTP, whatever went wrong is in the status.
    switch (response.status) {
    case 0:
    case 200:
    case 201:
    case 202:
    case 204:
    case 206:
    case 207:
        return VfsError::ok();
    case 301:
    case 302:
    case 307:
    case 308:
        return fail(VfsError::NetworkError, QStringLiteral("unexpected redirect"));
    case 401:
    case 403:
        return fail(VfsError::AccessDenied, QStringLiteral("the server refused the credentials"));
    case 404:
    case 410:
        return fail(VfsError::NotFound, QStringLiteral("no such file or directory"));
    case 405:
    case 501:
        return fail(VfsError::NotSupported, QStringLiteral("the server does not offer this operation"));
    case 409:
        return fail(VfsError::NotFound, QStringLiteral("a parent directory is missing"));
    case 412:
    case 423:
        return fail(VfsError::AccessDenied, QStringLiteral("the target is locked"));
    case 507:
    case 413:
        return fail(VfsError::IoError, QStringLiteral("no room left on the server"));
    default:
        return fail(VfsError::IoError, QStringLiteral("the server answered %1").arg(response.status));
    }
}

// ---- path encoding ---------------------------------------------------------

QByteArray encodeSegment(const QString& segment)
{
    // Everything a server could read as structure is encoded; the unreserved set
    // from RFC 3986 is left alone.
    return QUrl::toPercentEncoding(segment, QByteArray(), QByteArray("/"));
}

QByteArray encodePath(const QString& path)
{
    QByteArray out;
    const QStringList parts = path.split(QLatin1Char('/'));
    for (int i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out.append('/');
        out.append(encodeSegment(parts.at(i)));
    }
    return out;
}

} // namespace mole::net
