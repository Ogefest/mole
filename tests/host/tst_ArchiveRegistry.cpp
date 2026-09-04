#include "host/ArchiveRegistry.h"
#include "support/MoleTestMain.h"

using namespace mole;

namespace {

/// An archiver that writes whatever it was told it writes.
///
/// The registry's own rules are what is in doubt here -- which formats exist,
/// who gets a request, what happens when two plugins claim one kind -- and none
/// of that needs libarchive or a file. See ADR-0101.
class FakeArchiver final : public IArchiver
{
public:
    explicit FakeArchiver(QList<Format> formats)
        : m_formats(std::move(formats))
    {
    }

    QList<Format> formats() const override { return m_formats; }

    bool compress(const Request& request) override
    {
        m_asked.append(request);
        return m_answer;
    }

    void refuse() { m_answer = false; }
    const QList<Request>& asked() const { return m_asked; }

private:
    QList<Format> m_formats;
    QList<Request> m_asked;
    bool m_answer = true;
};

IArchiver::Format formatNamed(
    const QString& id, const QString& suffix, bool password = false, bool oneFileOnly = false)
{
    IArchiver::Format format;
    format.id = id;
    format.suffix = suffix;
    format.takesPassword = password;
    format.holdsOneFileOnly = oneFileOnly;
    return format;
}

} // namespace

class TestArchiveRegistry : public QObject
{
    Q_OBJECT

private slots:
    void nothingRegisteredMeansNothingCanBePacked();
    void whatIsRegisteredIsWhatIsOffered();
    void aFormatIsWrittenByWhicheverPluginClaimedIt();
    void aSecondClaimOnOneFormatIsRefused();
    void anArchiverWithNothingToOfferIsRefused();
    void aFormatNobodyWritesIsRefusedRatherThanGuessed();
};

void TestArchiveRegistry::nothingRegisteredMeansNothingCanBePacked()
{
    // What a build without libarchive looks like: its archive plugin is not built,
    // so nothing registers here and the shell offers no compression. Empty is an
    // ordinary answer rather than a fault.
    ArchiveRegistry registry;
    QVERIFY(!registry.canCompress());
    QVERIFY(registry.formats().isEmpty());
    QVERIFY(registry.format(QStringLiteral("zip")).id.isEmpty());
    QVERIFY(!registry.compress({}));
}

void TestArchiveRegistry::whatIsRegisteredIsWhatIsOffered()
{
    ArchiveRegistry registry;
    QVERIFY(registry.addArchiver(std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QStringLiteral("zip"), QStringLiteral(".zip"), true),
            formatNamed(QStringLiteral("xz"), QStringLiteral(".xz"), false, true) })));
    QVERIFY(registry.canCompress());

    // In registration order, because the first is what a picker opens on.
    const QList<IArchiver::Format> offered = registry.formats();
    QCOMPARE(offered.size(), 2);
    QCOMPARE(offered.first().id, QStringLiteral("zip"));

    // And each one answers for itself, which is what the dialog asks: a box for a
    // password only where a password means something, and a warning about a
    // second file only where a second file cannot go.
    QVERIFY(registry.format(QStringLiteral("zip")).takesPassword);
    QVERIFY(!registry.format(QStringLiteral("zip")).holdsOneFileOnly);
    QVERIFY(!registry.format(QStringLiteral("xz")).takesPassword);
    QVERIFY(registry.format(QStringLiteral("xz")).holdsOneFileOnly);
    QCOMPARE(registry.format(QStringLiteral("xz")).suffix, QStringLiteral(".xz"));

    // A second plugin adds kinds rather than replacing them, which is the whole
    // reason this is an extension point rather than a table in the shell.
    QVERIFY(registry.addArchiver(std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QStringLiteral("zst"), QStringLiteral(".tar.zst")) })));
    QCOMPARE(registry.formats().size(), 3);
    QCOMPARE(registry.format(QStringLiteral("zst")).suffix, QStringLiteral(".tar.zst"));
}

void TestArchiveRegistry::aFormatIsWrittenByWhicheverPluginClaimedIt()
{
    ArchiveRegistry registry;
    auto first = std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QStringLiteral("zip"), QStringLiteral(".zip")) });
    auto second = std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QStringLiteral("zst"), QStringLiteral(".tar.zst")) });
    FakeArchiver* zip = first.get();
    FakeArchiver* zst = second.get();
    QVERIFY(registry.addArchiver(std::move(first)));
    QVERIFY(registry.addArchiver(std::move(second)));

    IArchiver::Request request;
    request.formatId = QStringLiteral("zst");
    request.sources.append(VfsUri::fromString(QStringLiteral("file:///tmp/one.txt")));
    request.target = VfsUri::fromString(QStringLiteral("file:///tmp/one.tar.zst"));
    QVERIFY(registry.compress(request));

    QCOMPARE(zst->asked().size(), 1);
    QCOMPARE(zst->asked().first().target.fileName(), QStringLiteral("one.tar.zst"));
    QVERIFY2(zip->asked().isEmpty(), "a request went to a plugin that does not write that kind");
}

void TestArchiveRegistry::aSecondClaimOnOneFormatIsRefused()
{
    // Two writers of `.zip` would leave "which one packs this" to registration
    // order, which is not an answer anybody could predict or debug.
    ArchiveRegistry registry;
    QVERIFY(registry.addArchiver(std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QStringLiteral("zip"), QStringLiteral(".zip")) })));
    QVERIFY(!registry.addArchiver(std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QStringLiteral("zip"), QStringLiteral(".zip2")) })));

    // And the one that was there keeps the format, rather than the refusal
    // leaving it half taken.
    QCOMPARE(registry.formats().size(), 1);
    QCOMPARE(registry.format(QStringLiteral("zip")).suffix, QStringLiteral(".zip"));
}

void TestArchiveRegistry::anArchiverWithNothingToOfferIsRefused()
{
    ArchiveRegistry registry;
    QVERIFY(!registry.addArchiver(std::make_unique<FakeArchiver>(QList<IArchiver::Format> {})));
    QVERIFY(!registry.addArchiver(nullptr));
    // A format with no id could never be chosen, so it is not a format.
    QVERIFY(!registry.addArchiver(std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QString(), QStringLiteral(".zip")) })));
    QVERIFY(!registry.canCompress());
}

void TestArchiveRegistry::aFormatNobodyWritesIsRefusedRatherThanGuessed()
{
    // The shell reports this; what must not happen is a `.tar.bz2` written as a
    // zip under a name that says otherwise.
    ArchiveRegistry registry;
    auto only = std::make_unique<FakeArchiver>(
        QList<IArchiver::Format> { formatNamed(QStringLiteral("zip"), QStringLiteral(".zip")) });
    FakeArchiver* watched = only.get();
    QVERIFY(registry.addArchiver(std::move(only)));

    IArchiver::Request request;
    request.formatId = QStringLiteral("tar.bz2");
    request.sources.append(VfsUri::fromString(QStringLiteral("file:///tmp/one.txt")));
    request.target = VfsUri::fromString(QStringLiteral("file:///tmp/one.tar.bz2"));
    QVERIFY(!registry.compress(request));
    QVERIFY(watched->asked().isEmpty());
}

MOLE_TEST_MAIN(TestArchiveRegistry)
#include "tst_ArchiveRegistry.moc"
