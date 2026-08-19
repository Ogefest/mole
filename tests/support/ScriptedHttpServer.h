#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class QTcpServer;
class QTcpSocket;

namespace mole::test {

/// An HTTP server that answers wrongly, on purpose.
///
/// The live testbed answers the way a server should, which is what makes it
/// useful and also what makes it useless here. A server that claims a
/// `Content-Length` it does not deliver, refuses a chunked `PUT` with 411,
/// redirects a body somewhere else, or cuts a listing off half way through an
/// XML document cannot be arranged on demand — and those are the answers a file
/// manager has to survive, because somewhere out there is an appliance that
/// gives them.
///
/// So they are scripted instead. Every scenario here runs offline, in
/// milliseconds, on every change — which is the difference between knowing this
/// works and hoping it does.
///
/// **It runs on a thread of its own with no event loop.** `curl_easy_perform`
/// blocks the thread that calls it, so a server sharing that thread would never
/// get as far as accepting the connection. Qt's blocking socket calls —
/// `waitForNewConnection`, `waitForReadyRead` — need no event loop, which is
/// exactly what a test wants: no `QTest::qWait`, no processEvents, no ordering
/// to get wrong.
class ScriptedHttpServer
{
public:
    /// What arrived, so a test can assert on what was asked as well as on what
    /// the caller made of the answer.
    struct Request
    {
        QByteArray method;
        QByteArray path;
        QList<QByteArray> headers;
        QByteArray body;

        /// The value of a header, empty when it is absent. Case-insensitive,
        /// because HTTP is and a test that is not would be testing curl's
        /// capitalisation.
        QByteArray header(const QByteArray& name) const;
        bool isChunked() const;
    };

    /// What to send back. Each field is a distinct way of being wrong.
    struct Reply
    {
        int status = 200;
        QByteArray reason = "OK";
        /// Whole header lines, without the terminator: `Content-Type: text/xml`.
        QList<QByteArray> headers;
        QByteArray body;

        /// What `Content-Length` will claim. Negative means the truth, which is
        /// the ordinary case; anything else is the lie being tested.
        qint64 claimedLength = -1;

        /// Close the connection after this much of the body has gone out.
        /// Negative sends all of it. This is a transfer that stops early
        /// without ever saying so — the failure that looks like success.
        qint64 hangUpAfter = -1;

        /// Send this much of the body and then hold the connection open,
        /// sending nothing, for this long. Negative does neither.
        ///
        /// The neighbour of hangUpAfter, and the harder case: a connection that
        /// hangs up is a failure the client is told about, and one that goes
        /// quiet and stays open is not. Nothing arrives, nothing closes, and
        /// whatever is bounding the transfer has to be the thing that ends it.
        qint64 goQuietAfter = -1;
        int stayQuietMs = 0;

        /// Whether to read the request body before answering. A server
        /// answering 411 does not, and a caller has to cope with being refused
        /// before it has finished talking.
        bool readRequestBody = true;
    };

    /// Runs on the server's thread, once per request. Keep it to deciding what
    /// to answer; anything it touches is touched from another thread.
    using Handler = std::function<Reply(const Request&)>;

    explicit ScriptedHttpServer(Handler handler);
    ~ScriptedHttpServer();

    ScriptedHttpServer(const ScriptedHttpServer&) = delete;
    ScriptedHttpServer& operator=(const ScriptedHttpServer&) = delete;

    /// False when no port could be taken, which is worth failing a test over
    /// rather than timing out later.
    bool start();
    void stop();

    /// `http://127.0.0.1:<port>`, once started.
    QString url() const;

    /// Everything that arrived, in order.
    QList<Request> received() const;

private:
    void serve(QTcpServer& server);
    void answerOne(QTcpSocket& socket);

    Handler m_handler;
    /// Owned by the serving thread, which is also where it is created. A
    /// QTcpServer listening on one thread and accepting on another parents every
    /// connection into the wrong one, and Qt says so once per request.
    std::thread m_thread;
    std::atomic<bool> m_stopping { false };
    quint16 m_port = 0;

    mutable std::mutex m_mutex;
    QList<Request> m_received;
};

} // namespace mole::test
