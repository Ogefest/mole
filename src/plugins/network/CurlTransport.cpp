#include "plugins/network/CurlTransport.h"

#include <QUrl>

#include <cstring>

namespace mole::net {
namespace {

    std::once_flag g_initOnce;

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

    /// Polled by libcurl during the transfer; a non-zero return aborts it. This
    /// is what turns a CancelToken into a transfer that actually stops, rather
    /// than one that finishes and is then thrown away.
    int reportProgress(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const auto* cancel = static_cast<const CancelToken*>(userData);
        return cancel->isCancelled() ? 1 : 0;
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

CurlPool::Lease::Lease(CurlPool* pool, CURL* handle)
    : m_pool(pool)
    , m_handle(handle)
{
}

CurlPool::Lease::~Lease()
{
    if (m_pool && m_handle)
        m_pool->give(m_handle);
}

CurlPool::Lease::Lease(Lease&& other) noexcept
    : m_pool(other.m_pool)
    , m_handle(other.m_handle)
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
        other.m_pool = nullptr;
        other.m_handle = nullptr;
    }
    return *this;
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
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, m_options.stallBytesPerSecond);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, m_options.stallSeconds);
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
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, reportProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &cancel);

    response.code = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);

    // Detached before the handle goes back to the pool: the buffer is on our
    // stack and must not outlive this call.
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, nullptr);

    if (response.code != CURLE_OK) {
        response.detail = errorBuffer[0] != 0 ? QString::fromUtf8(errorBuffer)
                                              : QString::fromUtf8(curl_easy_strerror(response.code));
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
