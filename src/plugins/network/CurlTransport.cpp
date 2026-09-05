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

QByteArray withoutSecrets(const QByteArray& line)
{
    // An FTP control command first, because that is the one this missed. libcurl
    // traces them as HEADER_OUT -- `> USER alice`, `> PASS s3cret` -- and a
    // command has no colon in it, so the header rule below let the line through
    // verbatim. MOLE_LOG=curl, turned up to diagnose an FTP problem, wrote the
    // password into the session log that ADR-0012 says gets sent to whoever is
    // helping. ACCT is the same, and so is PASS on any other protocol curl
    // speaks that way. See MOLE-349.
    const QByteArray head = line.left(5).toUpper();
    if (head.startsWith("PASS ") || head.startsWith("ACCT "))
        return line.left(4) + " <redacted>";

    const int colon = line.indexOf(':');
    if (colon <= 0)
        return line;
    const QByteArray name = line.left(colon).toLower();
    if (name == "authorization" || name == "proxy-authorization" || name == "x-amz-security-token")
        return line.left(colon) + ": <redacted>";
    return line;
}

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

    /// The payload of an upload, and whether reading it failed.
    ///
    /// The flag is why this is a struct. Aborting the transfer is the only thing
    /// a read callback can do, and libcurl reports that as
    /// CURLE_ABORTED_BY_CALLBACK -- **the same code as the user pressing
    /// Cancel** -- so a staging-disk read error on an upload was reported as
    /// "Writing X was cancelled". See MOLE-373.
    size_t readFromDevice(char* buffer, size_t size, size_t count, void* userData)
    {
        auto* payload = static_cast<CurlPool::Lease::PayloadState*>(userData);
        if (!payload || !payload->device)
            return CURL_READFUNC_ABORT;
        const qint64 read = payload->device->read(buffer, static_cast<qint64>(size * count));
        if (read < 0) {
            payload->readFailed = true;
            payload->reason = payload->device->errorString().isEmpty()
                ? QStringLiteral("the file being sent could not be read")
                : payload->device->errorString();
            return CURL_READFUNC_ABORT;
        }
        return static_cast<size_t>(read);
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
    /// What the progress callback needs, which since perform() took over the
    /// loop is only the token.
    ///
    /// It used to carry a StallWatch built with zero patience -- so switched off
    /// -- and a `stalled` flag nothing read: machinery for a decision that moved
    /// to the loop and left its shape behind. See MOLE-369.
    struct ProgressWatch
    {
        const CancelToken* cancel = nullptr;
    };

    /// Polled by libcurl during the transfer; a non-zero return aborts it.
    ///
    /// It is here because it is called more often than perform()'s poll comes
    /// round, so a cancellation takes effect sooner. It decides nothing about
    /// stalling: the loop does that, against the handle's own counters.
    int reportProgress(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        auto* watch = static_cast<ProgressWatch*>(userData);
        return watch && watch->cancel && watch->cancel->isCancelled() ? 1 : 0;
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

namespace {

    /// The share's locks, taken and released by libcurl around its cache.
    ///
    /// A share is used from every thread that holds a lease, and libcurl does no
    /// locking of its own -- CURLSHOPT_LOCKFUNC exists for exactly this. One
    /// mutex per lockable kind rather than one for the lot, because libcurl may
    /// hold one while it takes another.
    void lockShare(CURL*, curl_lock_data data, curl_lock_access, void* userData)
    {
        auto* guards = static_cast<std::recursive_mutex*>(userData);
        guards[int(data) % 8].lock();
    }

    void unlockShare(CURL*, curl_lock_data data, void* userData)
    {
        auto* guards = static_cast<std::recursive_mutex*>(userData);
        guards[int(data) % 8].unlock();
    }

} // namespace

CurlPool::CurlPool(TransportOptions options)
    : m_options(std::move(options))
{
    ensureCurlInitialised();

    // The connection cache, kept where it can outlive a transfer's multi handle.
    // See the note on m_share: without this every transfer opened a connection
    // of its own, whatever the pool of easy handles did.
    m_share = curl_share_init();
    if (m_share) {
        curl_share_setopt(m_share, CURLSHOPT_LOCKFUNC, lockShare);
        curl_share_setopt(m_share, CURLSHOPT_UNLOCKFUNC, unlockShare);
        curl_share_setopt(m_share, CURLSHOPT_USERDATA, m_shareGuards);
        curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        // The DNS cache and the TLS session cache as well: a drive is one host,
        // so both are answered once rather than per transfer. Neither carries
        // credentials, and both are dropped with the pool.
        curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
}

CurlPool::~CurlPool()
{
    // The handles first: a share must outlive every easy handle using it, and
    // curl_share_cleanup() refuses while one still does.
    for (CURL* handle : m_idle)
        curl_easy_cleanup(handle);
    m_idle.clear();
    if (m_share)
        curl_share_cleanup(m_share);
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

    // Before anything else, because it is what makes the rest of this handle's
    // connection worth setting up: the cache it draws from lives on the pool
    // rather than on the multi perform() builds and throws away.
    if (m_share)
        curl_easy_setopt(handle, CURLOPT_SHARE, m_share);

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

    if (!m_options.privateKeyPath.isEmpty()) {
        curl_easy_setopt(handle, CURLOPT_SSH_PRIVATE_KEYFILE, m_options.privateKeyPath.toUtf8().constData());
        if (!m_options.privateKeyPassphrase.isEmpty()) {
            curl_easy_setopt(handle, CURLOPT_KEYPASSWD, m_options.privateKeyPassphrase.toUtf8().constData());
        }
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
    // On the lease, because it has to outlive this call and be readable from
    // perform(): a read that failed has to be told apart from a cancel.
    lease.m_payload = Lease::PayloadState { &payload, false, QString() };
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, readFromDevice);
    curl_easy_setopt(handle, CURLOPT_READDATA, &lease.m_payload);
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
    ProgressWatch watch { &cancel };
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
    bool multiFailed = false;
    int running = 1;

    while (running > 0) {
        const CURLMcode stepped = curl_multi_perform(multi, &running);
        if (stepped != CURLM_OK) {
            // **And a code.** This stored the text and broke, and response.code
            // is only ever set from a CURLMSG_DONE that never comes -- so it
            // stayed CURLE_OK, errorFor() answered ok(), and the half-driven
            // connection went back to the pool. A sendSpan() that hit it would
            // have committed a partial write of a file that was never sent.
            // See MOLE-373.
            response.code = CURLE_RECV_ERROR;
            response.detail = QString::fromUtf8(curl_multi_strerror(stepped));
            multiFailed = true;
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

    if (cancelled || stalled || multiFailed) {
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
    response.connectionsOpened = connects;

    if (sink) {
        // Only for a download into a device, which is the one case where both
        // numbers mean the same thing. A HEAD -- runCommand's NOBODY, S3's
        // stat -- is told the length and asks for none of it, and comparing the
        // two there would call every one of them truncated.
        response.expectedBytes = announced;
        response.receivedBytes = received;
    }

    // A payload that could not be read. libcurl can only be told to abort, and
    // it reports that as CURLE_ABORTED_BY_CALLBACK -- the user's Cancel -- so
    // this is turned back into what it was before anybody reads the code.
    if (lease.payloadFailed() && !cancelled) {
        response.code = CURLE_READ_ERROR;
        response.detail = lease.payloadFailure();
        lease.abandon();
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

    // Debug, not a warning: errorFor() below turns exactly this condition into an
    // IoError carrying both numbers, the task fails with it, and Task::execute()
    // writes that as a warning with the job's title on it. Warning here as well
    // made one short download look like two faults, and a reader had to notice
    // the numbers were the same to know it was one. The line stays for
    // MOLE_LOG=net, where it says which transfer of the run it was. See MOLE-407
    // and ADR-0012's amendment of 2026-09-04.
    //
    // Gated on `sink` for the same reason the error is: a HEAD is told the length
    // and asks for none of it, so without this every stat on an object store
    // announces a truncated download that never happened.
    if (sink && response.code == CURLE_OK && announced > 0 && received < announced) {
        qCDebug(networkLog, "#%llu %s ended after %lld of the %lld bytes it was promised",
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

namespace {

    /// What CURLE_QUOTE_ERROR turned out to be, read out of libcurl's own words.
    ///
    /// **Code 21 is every failing SFTP mkdir, rm, rmdir and rename, and every
    /// failing FTP MKD, DELE, RMD, RNFR and RNTO.** The code says only "a quote
    /// command failed"; the reason is in the error buffer, and errorFor() had no
    /// case for it at all -- so all of those were Unknown, "the answer that
    /// breaks every one of them" in the conformance suite's words. The backends
    /// pre-check with stat(), which catches the common cases, and leaves exactly
    /// the ones nobody can pre-check: a permission problem, a rename race, an
    /// rmdir of a directory that gained a child, a quota.
    ///
    /// Both protocols put a numeric reply in the text -- SFTP passes libssh2's
    /// message through, FTP its own three digits -- so this reads words rather
    /// than parsing a grammar neither of them promises. Never Unknown: an
    /// unrecognised reason is an I/O error, which is what it is.
    VfsError::Code quoteFailureFrom(const QString& detail)
    {
        const QString said = detail.toLower();
        const auto mentions = [&said](const char* text) { return said.contains(QLatin1String(text)); };

        if (mentions("permission denied") || mentions("access denied") || mentions("access is denied")
            || mentions("550") || mentions("553") || mentions("not allowed") || mentions("read-only")
            || mentions("read only")) {
            return VfsError::AccessDenied;
        }
        if (mentions("no such file") || mentions("does not exist") || mentions("not found")
            || mentions("no such directory")) {
            return VfsError::NotFound;
        }
        if (mentions("already exists") || mentions("file exists") || mentions("exists")) {
            return VfsError::AlreadyExists;
        }
        if (mentions("not empty") || mentions("directory not empty"))
            return VfsError::NotEmpty;
        if (mentions("quota") || mentions("no space") || mentions("disk full") || mentions("552"))
            return VfsError::IoError;
        return VfsError::IoError;
    }

} // namespace

VfsError errorFor(
    const Response& response, const QString& what, StatusMeaning meaning, Precondition precondition)
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
    // The server answered the request for the file with a refusal to send it.
    // libcurl distinguishes it from a missing file and nothing above needs to.
    case CURLE_FTP_COULDNT_RETR_FILE:
        return fail(VfsError::NotFound, detail);
    case CURLE_REMOTE_ACCESS_DENIED:
    case CURLE_LOGIN_DENIED:
    case CURLE_AUTH_ERROR:
        return fail(VfsError::AccessDenied, detail);
    case CURLE_REMOTE_FILE_EXISTS:
        return fail(VfsError::AlreadyExists, detail);
    // Every failing SFTP and FTP command that is not one of the above. Read out
    // of the text, and never Unknown -- see quoteFailureFrom().
    case CURLE_QUOTE_ERROR:
        return fail(quoteFailureFrom(response.detail), detail);
    case CURLE_REMOTE_DISK_FULL:
    case CURLE_WRITE_ERROR:
    case CURLE_READ_ERROR:
    case CURLE_UPLOAD_FAILED:
    // A body that stopped short. The Content-Length check below reports exactly
    // this as IoError when it is the one that catches it, so the two ways of
    // finding out the same thing now answer the same way -- it used to be
    // IoError one way and Unknown the other.
    case CURLE_PARTIAL_FILE:
    case CURLE_GOT_NOTHING:
    // A range the server would not honour, or a resume it could not make. Not a
    // network fault and not a missing file: the request could not be expressed
    // against this object.
    case CURLE_RANGE_ERROR:
    case CURLE_BAD_DOWNLOAD_RESUME:
    // The body could not be replayed for a retry -- a stream being written as it
    // is sent has already given its bytes away. Local, and an I/O error is what
    // every caller does about it.
    case CURLE_SEND_FAIL_REWIND:
    case CURLE_INTERFACE_FAILED:
    case CURLE_FILESIZE_EXCEEDED:
    // Never reported by a transfer that ran: it is how a backend says the
    // transfer could not be started at all, which is a local failure and not
    // something the other end did.
    case CURLE_FAILED_INIT:
    case CURLE_OUT_OF_MEMORY:
    // Building the request went wrong inside libcurl, and only the socket
    // interface -- which nothing here uses -- can answer CURLE_AGAIN. Both are
    // local and neither is anything the other end did.
    case CURLE_HTTP_POST_ERROR:
    case CURLE_AGAIN:
        return fail(VfsError::IoError, detail);
    // A local file the file: protocol could not open. Not reachable from the
    // backends here, and it is a missing file wherever it is.
    case CURLE_FILE_COULDNT_READ_FILE:
        return fail(VfsError::NotFound, detail);
    // A body in an encoding this build cannot decode.
    case CURLE_BAD_CONTENT_ENCODING:
        return fail(VfsError::NotSupported, detail);
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_NOT_BUILT_IN:
    case CURLE_UNKNOWN_OPTION:
        return fail(VfsError::NotSupported, detail);
    // An address nothing can be done with. NotSupported rather than NetworkError:
    // retrying it is pointless, which is exactly what NotSupported tells the
    // retry rule.
    case CURLE_URL_MALFORMAT:
        return fail(VfsError::NotSupported, detail);
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    // The certificate chain. PEER_FAILED_VERIFICATION was already here and its
    // neighbours were not, so the same refusal was NetworkError or Unknown
    // depending on which check inside OpenSSL noticed.
    // CURLE_SSL_CACERT is an alias for CURLE_PEER_FAILED_VERIFICATION above in
    // every libcurl this builds against, so it is not listed twice.
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_SHUTDOWN_FAILED:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_USE_SSL_FAILED:
    // The SSH layer: a host key that changed, a key file that will not load, a
    // channel that would not open. All of them are this connection, not this
    // file.
    case CURLE_SSH:
    case CURLE_TOO_MANY_REDIRECTS:
    case CURLE_WEIRD_SERVER_REPLY: // CURLE_FTP_WEIRD_SERVER_REPLY under its older name
    case CURLE_FTP_ACCEPT_FAILED:
    case CURLE_FTP_ACCEPT_TIMEOUT:
    case CURLE_FTP_CANT_GET_HOST:
    case CURLE_FTP_WEIRD_PASV_REPLY:
    case CURLE_FTP_WEIRD_PASS_REPLY:
    case CURLE_FTP_WEIRD_227_FORMAT:
    case CURLE_FTP_PORT_FAILED:
    case CURLE_FTP_COULDNT_SET_TYPE:
    case CURLE_FTP_COULDNT_USE_REST:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
    case CURLE_HTTP3:
    case CURLE_QUIC_CONNECT_ERROR:
        return fail(VfsError::NetworkError, detail);
    default:
        // Deliberately still here, and deliberately never reached by anything a
        // transfer in this application can return: tst_CurlTransport holds a list
        // of those against this function and fails if one of them lands here.
        // The default is for a libcurl newer than the one this was written
        // against.
        return fail(VfsError::Unknown, detail);
    }

    // A transfer curl called successful can still be short. libssh2 reports a
    // dropped SFTP channel as a clean end of file, so curl believes the download
    // finished and hands back whatever arrived; the same is true of an HTTP
    // response cut off after its Content-Length was read. Both look exactly like
    // a complete file to everything above, which is the worst possible outcome
    // of a copy -- so the one number that disproves it is checked here, for
    // every protocol, before anybody is told the file is good.
    //
    // Not for a status that carries an error document rather than the file: a
    // 416's body is XML about the range, and comparing its length to the file's
    // would call every one of them truncated.
    const bool carriesThePayload = response.status < 400;
    if (carriesThePayload && response.expectedBytes > 0 && response.receivedBytes >= 0
        && response.receivedBytes < response.expectedBytes) {
        return fail(VfsError::IoError,
            QStringLiteral("the transfer stopped after %1 of %2 bytes")
                .arg(response.receivedBytes)
                .arg(response.expectedBytes));
    }

    // A loop that broke without a CURLMSG_DONE leaves the code at CURLE_OK and
    // a sentence in `detail`. Answering ok() there put a half-driven connection
    // back in the pool and told the caller its write had landed. See
    // CurlPool::perform, which now sets a code -- this is the belt as well.
    if (response.code == CURLE_OK && response.status == 0 && !response.detail.isEmpty())
        return fail(VfsError::IoError, detail);

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
    // The range asked for starts past the end of the object, which is what the
    // end of a file looks like to a reader asking span by span. Answered as
    // success with nothing in it, so the streaming layer reads it the way it
    // reads a short span: as the end. It used to be "the server answered 416",
    // an IoError, retried for the whole two-minute budget first.
    case 416:
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
        // Only where the caller asked for it. A MOVE with `Overwrite: F` is
        // told 412 to mean "the destination is there", which is AlreadyExists;
        // rename() special-cased that itself and every other caller got
        // "the target is locked" for a condition it never set.
        return precondition == Precondition::Sent
            ? fail(VfsError::AlreadyExists, QStringLiteral("there is already something there"))
            : fail(VfsError::AccessDenied, QStringLiteral("the server would not meet the condition"));
    case 423:
        return fail(VfsError::AccessDenied, QStringLiteral("the target is locked"));
    case 507:
        return fail(VfsError::IoError, QStringLiteral("no room left on the server"));
    // **A 413 is not a full disk, and this said it was.** Both used to answer
    // "no room left on the server", and they are different answers with
    // different remedies: 507 is a destination that is full -- free space and
    // try again -- where 413 is the server refusing a request of this size
    // whatever its disk holds. Found on the heavy tier: a 1.82 GiB upload
    // stopped at 1.01 GiB saying there was no room, against a destination the
    // same run had just measured as having 3.64 GiB free. It was Apache's
    // LimitRequestBody, which has defaulted to 1 GiB since httpd 2.4.54 and was
    // in nobody's configuration file. Somebody reading the old message frees
    // space and hits it again.
    //
    // NotSupported rather than IoError, and that is the half that matters as
    // much as the words: IoError is retried -- see isWorthRetrying() below --
    // and a request refused for its size is refused identically every time. A
    // WebDAV upload is one request, because the protocol has no ranged PUT, so
    // there is not even a smaller piece to send. See MOLE-327.
    case 413:
        return fail(VfsError::NotSupported,
            QStringLiteral("the server refuses a request this large (413). It is a limit on the "
                           "request rather than room on the disk, so freeing space will not help: "
                           "the file has to be smaller, or the server's limit larger"));
    // The server is there and cannot serve this now. **Not a disk error**: a
    // gateway that is down, an S3 SlowDown, a proxy timing out and a rate limit
    // are the same conditions as a socket that would not open, and every one of
    // them was reported as "the server answered 503" -- an IoError, which the
    // sidebar and the copy layer both read as something wrong with the storage.
    case 408:
    case 429:
    case 502:
    case 503:
    case 504:
        return fail(VfsError::NetworkError,
            QStringLiteral("the server is not answering requests just now (%1)").arg(response.status));
    default:
        return fail(VfsError::IoError, QStringLiteral("the server answered %1").arg(response.status));
    }
}

bool isWorthRetrying(const VfsError& error)
{
    switch (error.code) {
    // The connection, which is the case retrying exists for.
    case VfsError::NetworkError:
    // A transfer that stopped part way. The other end may well finish it next
    // time, and a truncated body arrives here.
    case VfsError::IoError:
    // A code this build does not recognise. Trying again is the safer guess
    // about something nobody has classified.
    case VfsError::Unknown:
        return true;
    case VfsError::None:
    case VfsError::NotFound:
    case VfsError::AccessDenied:
    case VfsError::NotSupported:
    case VfsError::NotADirectory:
    case VfsError::IsADirectory:
    case VfsError::NotALink:
    case VfsError::AlreadyExists:
    case VfsError::NotEmpty:
    case VfsError::Cancelled:
        // Every one of these is the same answer next time. Waiting two minutes
        // to say it again is two minutes per file.
        return false;
    }
    return false;
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

QDateTime httpDate(const QString& text)
{
    QString normalised = text.trimmed();

    // The weekday goes first. It carries no information -- the date is fully
    // specified without it -- and Qt *validates* it, so a server whose
    // arithmetic disagrees by a day has its timestamp rejected outright rather
    // than being off by a little. Dropping it cannot lose anything.
    const int comma = normalised.indexOf(QLatin1Char(','));
    if (comma >= 0 && comma <= 4)
        normalised = normalised.mid(comma + 1).trimmed();

    // The zone spelled out as a name, turned into the offset Qt will read.
    if (normalised.endsWith(QLatin1String("GMT"), Qt::CaseInsensitive)
        || normalised.endsWith(QLatin1String("UTC"), Qt::CaseInsensitive)) {
        normalised.chop(3);
        normalised = normalised.trimmed() + QStringLiteral(" +0000");
    }

    QDateTime stamp = QDateTime::fromString(normalised, Qt::RFC2822Date);
    // And the two other shapes a server may answer with: ISO 8601, which is
    // what WebDAV's creationdate and S3's listings use, with and without
    // fractional seconds.
    if (!stamp.isValid())
        stamp = QDateTime::fromString(text, Qt::RFC2822Date);
    if (!stamp.isValid())
        stamp = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!stamp.isValid())
        stamp = QDateTime::fromString(text, Qt::ISODate);
    return stamp;
}

} // namespace mole::net
