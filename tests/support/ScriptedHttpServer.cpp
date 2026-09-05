#include "support/ScriptedHttpServer.h"

#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>

#include <future>

namespace {

/// A listener that hands over descriptors rather than sockets, so each
/// conversation can build its socket on the thread that will use it.
class Accepting final : public QTcpServer
{
public:
    std::mutex guard;
    std::vector<qintptr> pending;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        const std::lock_guard<std::mutex> held(guard);
        pending.push_back(descriptor);
    }
};

} // namespace

namespace mole::test {
namespace {

    /// Long enough that a loaded machine does not fail a test, short enough that
    /// a genuinely stuck socket does not hold the suite up for a minute.
    constexpr int kSocketWaitMs = 10000;

    QByteArray trimmed(const QByteArray& value)
    {
        return value.trimmed();
    }

} // namespace

QByteArray ScriptedHttpServer::Request::header(const QByteArray& name) const
{
    for (const QByteArray& line : headers) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        if (line.left(colon).trimmed().toLower() == name.toLower())
            return trimmed(line.mid(colon + 1));
    }
    return {};
}

bool ScriptedHttpServer::Request::isChunked() const
{
    return header("Transfer-Encoding").toLower().contains("chunked");
}

ScriptedHttpServer::ScriptedHttpServer(Handler handler)
    : m_handler(std::move(handler))
{
}

ScriptedHttpServer::~ScriptedHttpServer()
{
    stop();
}

bool ScriptedHttpServer::start()
{
    std::promise<quint16> listening;
    std::future<quint16> port = listening.get_future();

    m_thread = std::thread([this, &listening] {
        Accepting server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            listening.set_value(0);
            return;
        }
        listening.set_value(server.serverPort());
        serve(server);
    });

    m_port = port.get();
    if (m_port == 0) {
        m_thread.join();
        return false;
    }
    return true;
}

void ScriptedHttpServer::stop()
{
    if (!m_thread.joinable())
        return;
    m_stopping = true;
    m_thread.join();
}

QString ScriptedHttpServer::url() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(m_port);
}

QList<ScriptedHttpServer::Request> ScriptedHttpServer::received() const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return m_received;
}

void ScriptedHttpServer::serve(QTcpServer& server)
{
    // **A thread per connection, because one at a time cannot answer two clients
    // at once** -- and with keep-alive it cannot even reach the second, since a
    // conversation stays on a socket the client is holding open for reuse. A
    // test of two transfers running together deadlocked against this fixture
    // rather than against anything it was testing. See MOLE-418.
    //
    // Descriptors rather than sockets: a QTcpSocket belongs to the thread that
    // made it, and Accepting hands over the number instead so each conversation
    // builds its own. That is what incomingConnection() is for.
    auto& accepting = static_cast<Accepting&>(server);
    std::vector<std::thread> conversations;
    while (!m_stopping) {
        // A short wait rather than a blocking accept, so stop() does not have to
        // poke the socket to be noticed.
        server.waitForNewConnection(50);
        std::vector<qintptr> arrived;
        {
            const std::lock_guard<std::mutex> guard(accepting.guard);
            arrived.swap(accepting.pending);
        }
        for (const qintptr descriptor : arrived) {
            conversations.emplace_back([this, descriptor] {
                QTcpSocket socket;
                if (!socket.setSocketDescriptor(descriptor, QAbstractSocket::ConnectedState))
                    return;
                // One answer, or several down the same socket when the handler
                // asks for keep-alive. A client that reuses connections cannot
                // be told apart from one that does not by a server that closes
                // every one.
                while (
                    answerOne(socket) && !m_stopping && socket.state() == QAbstractSocket::ConnectedState) { }
                socket.disconnectFromHost();
                if (socket.state() != QAbstractSocket::UnconnectedState)
                    socket.waitForDisconnected(1000);
            });
        }
    }

    // Every conversation finished before the server goes: a socket outliving it
    // would be answering out of a destroyed handler.
    for (std::thread& conversation : conversations) {
        if (conversation.joinable())
            conversation.join();
    }
}

bool ScriptedHttpServer::answerOne(QTcpSocket& socket)
{
    QByteArray head;
    while (!head.contains("\r\n\r\n")) {
        if (!socket.waitForReadyRead(kSocketWaitMs))
            return false;
        head += socket.readAll();
    }

    const int endOfHead = head.indexOf("\r\n\r\n");
    QByteArray rest = head.mid(endOfHead + 4);
    const QList<QByteArray> lines = head.left(endOfHead).split('\n');

    Request request;
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (i == 0) {
            const QList<QByteArray> parts = line.split(' ');
            if (parts.size() >= 2) {
                request.method = parts.at(0);
                request.path = parts.at(1);
            }
            continue;
        }
        if (!line.isEmpty())
            request.headers.append(line);
    }

    const Reply reply = m_handler(request);

    if (reply.readRequestBody) {
        const qint64 declared = request.header("Content-Length").toLongLong();
        if (request.isChunked()) {
            // Chunked ends with a zero-length chunk. Reading to it rather than
            // to a byte count is the only way to know the body is complete.
            while (!rest.contains("\r\n0\r\n")) {
                if (!socket.waitForReadyRead(kSocketWaitMs))
                    break;
                rest += socket.readAll();
            }
        } else {
            while (rest.size() < declared) {
                if (!socket.waitForReadyRead(kSocketWaitMs))
                    break;
                rest += socket.readAll();
            }
        }
        request.body = rest;
    }

    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_received.append(request);
    }

    const qint64 claimed = reply.claimedLength >= 0 ? reply.claimedLength : reply.body.size();

    QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + ' ' + reply.reason + "\r\n";
    for (const QByteArray& header : reply.headers)
        out += header + "\r\n";
    out += "Content-Length: " + QByteArray::number(claimed) + "\r\n";
    // Closing is the default, because keep-alive is one more thing to get right
    // in a fixture whose whole job is to be wrong in exactly one way at a time.
    // The case that is about connection reuse asks for the other behaviour.
    out += reply.keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";

    const qint64 send
        = reply.hangUpAfter >= 0 ? std::min<qint64>(reply.hangUpAfter, reply.body.size()) : reply.body.size();
    if (reply.goQuietAfter >= 0) {
        // Part of the body, then silence with the connection still open. The
        // caller is told nothing at all: no close, no error, no bytes.
        const qint64 first = std::min<qint64>(reply.goQuietAfter, reply.body.size());
        out += reply.body.left(static_cast<int>(first));
        socket.write(out);
        socket.waitForBytesWritten(kSocketWaitMs);
        // Held rather than slept through, so a caller that gives up early does
        // not leave this thread sitting on a socket nobody is reading.
        QElapsedTimer quiet;
        quiet.start();
        while (quiet.elapsed() < reply.stayQuietMs && socket.state() == QAbstractSocket::ConnectedState)
            socket.waitForReadyRead(50);
        return false;
    }

    out += reply.body.left(static_cast<int>(send));

    socket.write(out);
    socket.waitForBytesWritten(kSocketWaitMs);
    // Only a whole answer on a keep-alive connection can be followed by another:
    // a body cut short leaves the client's parser mid-message.
    return reply.keepAlive && reply.hangUpAfter < 0;
}

} // namespace mole::test
