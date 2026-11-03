#include "support/MoleTestMain.h"
#include "support/ScriptedHttpServer.h"

#include <QTcpSocket>
#include <QUrl>

using namespace mole::test;

/// The scripted server, held to the one promise every HTTP suite leans on: it is
/// wrong in exactly the way a case asks for and in no other.
///
/// It was wrong in a way no case asked for. Four cases added a `Content-Length`
/// line of their own, the server added its own after it, and every answer they
/// got carried two different lengths. libcurl up to 8.x took the first and
/// carried on; 8.18 refuses the answer, which is what RFC 9110 asks of a client.
/// So the fixture was malformed and nothing said so until a newer curl did.
/// See MOLE-413.
class TestScriptedHttpServer : public QObject
{
    Q_OBJECT

private slots:
    void aReplyCarriesOneContentLength_data();
    void aReplyCarriesOneContentLength();

private:
    /// Every header line of the answer to one HEAD request, read off a raw
    /// socket so nothing between the fixture and the test can tidy it.
    static QList<QByteArray> headLinesFrom(const ScriptedHttpServer& server);
};

QList<QByteArray> TestScriptedHttpServer::headLinesFrom(const ScriptedHttpServer& server)
{
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(QUrl(server.url()).port()));
    if (!socket.waitForConnected(5000))
        return {};
    socket.write("HEAD /notes.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    socket.waitForBytesWritten(5000);

    QByteArray head;
    while (!head.contains("\r\n\r\n")) {
        if (!socket.waitForReadyRead(5000))
            break;
        head += socket.readAll();
    }
    return head.left(head.indexOf("\r\n\r\n")).split('\n');
}

void TestScriptedHttpServer::aReplyCarriesOneContentLength_data()
{
    QTest::addColumn<QByteArray>("rawHeader");
    QTest::addColumn<qint64>("claimed");
    QTest::addColumn<QByteArray>("expected");

    // The mechanism the fixture has for a length that is not the body's.
    QTest::newRow("claimedLength") << QByteArray() << qint64(12) << QByteArray("12");
    // What the four cases wrote instead: a line of their own. It is the claim,
    // and it replaces the fixture's rather than joining it.
    QTest::newRow("a raw header") << QByteArray("Content-Length: 12") << qint64(-1) << QByteArray("12");
    // HTTP header names are not case-sensitive, so neither is the check.
    QTest::newRow("a raw header in lower case")
        << QByteArray("content-length: 6442450944") << qint64(-1) << QByteArray("6442450944");
    // Neither: the truth, which for this empty body is nought.
    QTest::newRow("nothing said") << QByteArray() << qint64(-1) << QByteArray("0");
}

void TestScriptedHttpServer::aReplyCarriesOneContentLength()
{
    QFETCH(QByteArray, rawHeader);
    QFETCH(qint64, claimed);
    QFETCH(QByteArray, expected);

    ScriptedHttpServer server([&](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        if (!rawHeader.isEmpty())
            reply.headers.append(rawHeader);
        reply.headers.append("Last-Modified: Wed, 12 Oct 2022 10:00:00 GMT");
        reply.claimedLength = claimed;
        return reply;
    });
    QVERIFY2(server.start(), "the scripted server could not take a port");

    const QList<QByteArray> lines = headLinesFrom(server);
    QVERIFY2(!lines.isEmpty(), "no answer came back");

    QList<QByteArray> lengths;
    for (const QByteArray& line : lines) {
        if (line.trimmed().toLower().startsWith("content-length:"))
            lengths.append(line.mid(line.indexOf(':') + 1).trimmed());
    }
    QVERIFY2(lengths.size() == 1,
        qPrintable(QStringLiteral("the answer said its length %1 times: %2")
                       .arg(lengths.size())
                       .arg(QString::fromLatin1(lengths.join(", ")))));
    QCOMPARE(lengths.first(), expected);
    // And nothing else the case wrote went missing along with it.
    QVERIFY(std::any_of(lines.cbegin(), lines.cend(),
        [](const QByteArray& line) { return line.startsWith("Last-Modified:"); }));
}

MOLE_TEST_MAIN(TestScriptedHttpServer)
#include "tst_ScriptedHttpServer.moc"
