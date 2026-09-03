#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"

#include "core/diagnostics/Diagnostics.h"
#include "core/diagnostics/LoggingFileSystem.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/VersionGuard.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QBuffer>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;

namespace {

/// A drive that answers differently from the interface's default on every method
/// that has one.
///
/// No answer here can be arrived at by accident, and that is the whole design.
/// IFileSystem's defaults are deliberately the harmless-looking ones --
/// permissive name rules, case-sensitive paths, NotSupported, empty -- so a
/// decorator that forgets to forward a method does not fail: it answers
/// *plausibly*, on behalf of a drive that was never asked. A fake agreeing with
/// any default would let exactly that through, which is how MOLE-282 survived
/// four methods and two wrappers.
///
/// Stateless on purpose. Every case below asks the same drive twice -- once bare
/// and once through a wrapper -- and compares the two answers, so a method whose
/// second answer differed from its first because of what the first one did would
/// fail for a reason that has nothing to do with forwarding. probe() is the one
/// exception, and it is counted rather than compared.
class OpinionatedFileSystem final : public IFileSystem
{
public:
    static QString actionId() { return QStringLiteral("org.mole.test.opinion"); }

    QString scheme() const override { return QStringLiteral("opinion"); }

    VfsCapabilities capabilities() const override
    {
        return VfsCapability::Read | VfsCapability::Write | VfsCapability::ReportsSpace
            | VfsCapability::ReportsAccess | VfsCapability::ReportsLeftovers | VfsCapability::NativeSearch;
    }

    /// Not the default, which is Qt::CaseSensitive.
    Qt::CaseSensitivity pathCaseSensitivity() const override { return Qt::CaseInsensitive; }

    /// Not the default, which accepts everything.
    NameRules nameRules() const override
    {
        NameRules rules;
        rules.forbiddenCharacters = QStringLiteral("%@");
        rules.refusesControlCharacters = true;
        rules.refusesTrailingDotOrSpace = true;
        rules.refusesReservedDeviceNames = true;
        rules.maximumLength = 42;
        return rules;
    }

    /// Not the default, which is false -- and true keeps VersionGuard a
    /// pass-through, so its own refusal does not stand in for a lost forward.
    bool understandsVersions() const override { return true; }

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken&) override
    {
        FileEntry entry;
        entry.name = QStringLiteral("listed");
        entry.uri = dir;
        entry.size = 11;
        return Result<FileEntryList>(FileEntryList { entry });
    }

    Result<FileEntry> stat(const VfsUri& target) override
    {
        FileEntry entry;
        entry.name = QStringLiteral("stated");
        entry.uri = target;
        entry.size = 22;
        return Result<FileEntry>(entry);
    }

    Result<void> makeDirectory(const VfsUri&) override
    {
        return Result<void>::failure(VfsError::AlreadyExists, QStringLiteral("mkdir was here"));
    }

    Result<void> remove(const VfsUri&, bool recursive) override
    {
        return Result<void>::failure(VfsError::NotEmpty,
            recursive ? QStringLiteral("remove -r was here") : QStringLiteral("remove was here"));
    }

    Result<void> rename(const VfsUri&, const VfsUri&) override
    {
        return Result<void>::failure(VfsError::IsADirectory, QStringLiteral("rename was here"));
    }

    /// Not the default either, and the default is the one that would pass for an
    /// answer: it removes the destination and renames onto it, so a wrapper that
    /// lost this one would go on working -- against a drive that was never asked
    /// whether it could do better, and through two calls where the drive offers
    /// one. See ADR-0087.
    Result<void> replace(const VfsUri&, const VfsUri&) override
    {
        return Result<void>::failure(VfsError::NotEmpty, QStringLiteral("replace was here"));
    }

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri&, qint64 expectedSize) override
    {
        // The hint comes back in the bytes, so a wrapper that dropped it or
        // substituted its own default is a different answer rather than the
        // same one. The default binds to the static type: a decorator declaring
        // openRead() without it compiles and then hands -1 to the drive.
        auto buffer = std::make_unique<QBuffer>();
        buffer->setData(QByteArray("read hint ") + QByteArray::number(expectedSize));
        buffer->open(QIODevice::ReadOnly);
        return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(buffer.release()));
    }

    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri&, qint64 expectedSize) override
    {
        return Result<std::unique_ptr<QIODevice>>(
            VfsError::make(VfsError::AccessDenied, QStringLiteral("write hint %1").arg(expectedSize)));
    }

    Result<SpaceInfo> space(const VfsUri&) override
    {
        SpaceInfo info;
        info.totalBytes = 3003;
        info.freeBytes = 1001;
        return Result<SpaceInfo>(info);
    }

    Result<AccessInfo> access(const VfsUri&) override
    {
        AccessInfo info;
        info.read = AccessInfo::Answer::Yes;
        info.write = AccessInfo::Answer::No;
        info.nativeText = QStringLiteral("r--r--r--");
        info.owner = QStringLiteral("opinion");
        return Result<AccessInfo>(info);
    }

    Result<QList<DriveLeftover>> leftovers(std::chrono::seconds olderThan, const CancelToken&) override
    {
        // The threshold comes back in the handle for the same reason the read
        // hint does: it is a value a wrapper can quietly replace.
        DriveLeftover held;
        held.handle = QStringLiteral("held-%1").arg(static_cast<qlonglong>(olderThan.count()));
        held.path = QStringLiteral("/unfinished");
        held.bytes = 4004;
        held.what = QStringLiteral("an upload nobody was alive to finish");
        return Result<QList<DriveLeftover>>(QList<DriveLeftover> { held });
    }

    Result<void> discardLeftover(const DriveLeftover& leftover) override
    {
        // Answers from the handle alone, so asking twice answers twice the same.
        if (leftover.handle.startsWith(QStringLiteral("held-")))
            return {};
        return Result<void>::failure(VfsError::NotFound, QStringLiteral("no such leftover"));
    }

    Result<FileEntryList> search(const VfsUri& root, const QString& pattern, const CancelToken&) override
    {
        FileEntry entry;
        entry.name = QStringLiteral("found ") + pattern;
        entry.uri = root;
        return Result<FileEntryList>(FileEntryList { entry });
    }

    FileActionList actionsFor(const VfsUri&, const FileEntry& entry) override
    {
        FileAction action;
        action.id = actionId();
        action.title = QStringLiteral("Opinion about ") + entry.name;
        return FileActionList { action };
    }

    Result<FileActionOutcome> invoke(const QString& id, const VfsUri&, const CancelToken&) override
    {
        if (id != actionId())
            return Result<FileActionOutcome>::failure(VfsError::NotSupported, QStringLiteral("not mine"));
        return Result<FileActionOutcome>(FileActionOutcome::fromText(QStringLiteral("invoked")));
    }

    Result<QStringList> entriesWithActions(const VfsUri&, const CancelToken&) override
    {
        return Result<QStringList>(QStringList { QStringLiteral("marked") });
    }

    DriveOffers offers() const override
    {
        DriveOffers found;
        found.state = DriveOffers::State::Answered;
        found.ids = QStringList { actionId() };
        return found;
    }

    void probe(const VfsUri&, const CancelToken&) override { ++probes; }

    /// How many times probe() reached the drive. The one method that answers
    /// nothing, so the only one a comparison of answers cannot see.
    mutable int probes = 0;
};

/// Every answer the drive gives, in one comparable list.
///
/// A fingerprint rather than an assertion per method, because the claim being
/// made is "identically, on all of them" -- and a list of per-method assertions
/// is the maintained list the ticket is about. The failure prints the two lists
/// side by side, so which method was lost is still the first thing visible.
QStringList fingerprint(IFileSystem& fs, const OpinionatedFileSystem& drive)
{
    const VfsUri target = VfsUri::fromString(QStringLiteral("opinion:///a/file"));
    const VfsUri other = VfsUri::fromString(QStringLiteral("opinion:///b/file"));
    const CancelToken cancel;

    const auto said = [](const char* method, const QString& answer) {
        return QLatin1String(method) + QStringLiteral(" = ") + answer;
    };
    const auto outcome = [](const auto& result) {
        return result.ok()
            ? QStringLiteral("ok")
            : QStringLiteral("error %1 \"%2\"")
                  .arg(QString::number(static_cast<int>(result.error().code)), result.error().message);
    };

    QStringList lines;
    lines << said("scheme", fs.scheme());
    lines << said("capabilities", QString::number(static_cast<quint32>(fs.capabilities())));
    lines << said("pathCaseSensitivity", QString::number(static_cast<int>(fs.pathCaseSensitivity())));

    const NameRules rules = fs.nameRules();
    lines << said("nameRules",
        QStringLiteral("\"%1\" ctrl=%2 trailing=%3 devices=%4 max=%5")
            .arg(rules.forbiddenCharacters)
            .arg(rules.refusesControlCharacters)
            .arg(rules.refusesTrailingDotOrSpace)
            .arg(rules.refusesReservedDeviceNames)
            .arg(rules.maximumLength));

    lines << said("understandsVersions", QString::number(fs.understandsVersions()));

    const Result<FileEntryList> listed = fs.list(target, cancel);
    QStringList names;
    if (listed.ok()) {
        for (const FileEntry& entry : listed.value())
            names << QStringLiteral("%1/%2").arg(entry.name).arg(entry.size);
    }
    lines << said("list", outcome(listed) + QLatin1Char(' ') + names.join(QLatin1Char(',')));

    const Result<FileEntry> stated = fs.stat(target);
    lines << said("stat",
        outcome(stated)
            + (stated.ok() ? QStringLiteral(" %1/%2").arg(stated.value().name).arg(stated.value().size)
                           : QString()));

    lines << said("makeDirectory", outcome(fs.makeDirectory(target)));
    lines << said("remove", outcome(fs.remove(target, true)));
    lines << said("rename", outcome(fs.rename(target, other)));
    lines << said("replace", outcome(fs.replace(target, other)));

    // The hint is passed explicitly and then left out, because leaving it out is
    // the mistake the interface warns about: a default argument binds to the
    // static type, so an override that omits it silently sends -1 to the drive.
    Result<std::unique_ptr<QIODevice>> read = fs.openRead(target, 4096);
    lines << said("openRead(4096)",
        outcome(read)
            + (read.ok() ? QLatin1Char(' ') + QString::fromUtf8(read.value()->readAll()) : QString()));
    Result<std::unique_ptr<QIODevice>> defaulted = fs.openRead(target);
    lines << said("openRead()",
        outcome(defaulted)
            + (defaulted.ok() ? QLatin1Char(' ') + QString::fromUtf8(defaulted.value()->readAll())
                              : QString()));
    lines << said("openWrite(8192)", outcome(fs.openWrite(target, 8192)));
    lines << said("openWrite()", outcome(fs.openWrite(target)));

    const Result<SpaceInfo> room = fs.space(target);
    lines << said("space",
        outcome(room)
            + (room.ok() ? QStringLiteral(" %1/%2").arg(room.value().totalBytes).arg(room.value().freeBytes)
                         : QString()));

    const Result<AccessInfo> who = fs.access(target);
    lines << said("access",
        outcome(who)
            + (who.ok() ? QStringLiteral(" %1 %2 %3")
                              .arg(static_cast<int>(who.value().read))
                              .arg(who.value().nativeText, who.value().owner)
                        : QString()));

    const Result<QList<DriveLeftover>> held = fs.leftovers(std::chrono::seconds(90), cancel);
    QStringList handles;
    if (held.ok()) {
        for (const DriveLeftover& one : held.value())
            handles << QStringLiteral("%1/%2").arg(one.handle).arg(one.bytes);
    }
    lines << said("leftovers", outcome(held) + QLatin1Char(' ') + handles.join(QLatin1Char(',')));

    DriveLeftover known;
    known.handle = QStringLiteral("held-90");
    lines << said("discardLeftover", outcome(fs.discardLeftover(known)));

    const Result<FileEntryList> hits = fs.search(target, QStringLiteral("needle"), cancel);
    QStringList found;
    if (hits.ok()) {
        for (const FileEntry& entry : hits.value())
            found << entry.name;
    }
    lines << said("search", outcome(hits) + QLatin1Char(' ') + found.join(QLatin1Char(',')));

    FileEntry subject;
    subject.name = QStringLiteral("subject");
    QStringList actions;
    for (const FileAction& action : fs.actionsFor(target, subject))
        actions << action.id + QLatin1Char('/') + action.title;
    lines << said("actionsFor", actions.join(QLatin1Char(',')));

    const Result<FileActionOutcome> did = fs.invoke(OpinionatedFileSystem::actionId(), target, cancel);
    lines << said("invoke", outcome(did) + (did.ok() ? QLatin1Char(' ') + did.value().text : QString()));

    const Result<QStringList> marked = fs.entriesWithActions(target, cancel);
    lines << said("entriesWithActions",
        outcome(marked)
            + (marked.ok() ? QLatin1Char(' ') + marked.value().join(QLatin1Char(',')) : QString()));

    const DriveOffers offered = fs.offers();
    lines << said("offers",
        QStringLiteral("%1 %2").arg(static_cast<int>(offered.state)).arg(offered.ids.join(QLatin1Char(','))));

    // A void answer, so it is counted rather than compared: a delta of one means
    // the call reached the drive, and nought means the wrapper swallowed it.
    const int before = drive.probes;
    fs.probe(target, cancel);
    lines << said("probe", QStringLiteral("%1 call(s) reached the drive").arg(drive.probes - before));

    return lines;
}

/// Runs the whole fingerprint through `wrap`, twice: with the drive log off and
/// with it on.
///
/// Both, because LoggingFileSystem is two implementations of every method it
/// touches -- one that measures and one that does not -- and only the quiet
/// branch is ever exercised by an ordinary suite. A forward present in one and
/// missing in the other is exactly the shape of list(), which is written out
/// twice by hand.
void checkForwards(const char* what, const std::function<FileSystemPtr(FileSystemPtr)>& wrap)
{
    for (const bool logging : { false, true }) {
        QLoggingCategory::setFilterRules(logging ? QStringLiteral("mole.drive.debug=true") : QString());

        auto drive = std::make_shared<OpinionatedFileSystem>();
        const FileSystemPtr wrapped = wrap(drive);
        QVERIFY(wrapped != nullptr);

        const QStringList bare = fingerprint(*drive, *drive);
        const QStringList through = fingerprint(*wrapped, *drive);

        QVERIFY2(bare.size() > 20, qPrintable(QStringLiteral("only %1 answers asked").arg(bare.size())));
        QCOMPARE(through.size(), bare.size());

        // Only what differs, because testlib truncates a long message -- and a
        // dump of two whole fingerprints is exactly long enough to be cut off at
        // the line that differs. The names are on the left of each answer, so a
        // pair of lines says which method was lost.
        QStringList differences;
        for (qsizetype i = 0; i < bare.size(); ++i) {
            if (bare.at(i) != through.at(i)) {
                differences << QStringLiteral("the drive says      %1").arg(bare.at(i));
                differences << QStringLiteral("through the wrapper %1").arg(through.at(i));
            }
        }
        QVERIFY2(differences.isEmpty(),
            qPrintable(QStringLiteral("%1 answers for the drive it wraps, with the drive log %2:\n    %3")
                           .arg(QLatin1String(what), logging ? QStringLiteral("on") : QStringLiteral("off"),
                               differences.join(QStringLiteral("\n    ")))));
    }
    QLoggingCategory::setFilterRules(QString());
}

// ---- reading the headers ------------------------------------------------
//
// The behavioural cases above prove that what is declared really delegates. They
// cannot prove that everything is declared: a method added to IFileSystem
// tomorrow is not in the fake either, so nothing would ask for it. That is what
// these read the source for, and it is the half of the rule that survives
// somebody else's change.

/// The file, with every comment taken out.
///
/// Essential rather than tidy: the headers here name methods in their prose --
/// "still advertising ReportsLeftovers through capabilities() below" is a
/// sentence in LoggingFileSystem.h -- and a scan that counted those would report
/// a forward that does not exist.
QString withoutComments(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QStringList kept;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const qsizetype comment = line.indexOf(QStringLiteral("//"));
        kept << (comment >= 0 ? line.left(comment) : line);
    }
    return kept.join(QLatin1Char('\n'));
}

/// The identifier of the call whose closing bracket is at `close`.
QString methodEndingAt(const QString& text, qsizetype close)
{
    int depth = 0;
    qsizetype open = -1;
    for (qsizetype i = close; i >= 0; --i) {
        if (text.at(i) == QLatin1Char(')')) {
            ++depth;
        } else if (text.at(i) == QLatin1Char('(')) {
            if (--depth == 0) {
                open = i;
                break;
            }
        }
    }
    if (open <= 0)
        return {};

    qsizetype end = open - 1;
    while (end >= 0 && text.at(end).isSpace())
        --end;
    qsizetype start = end;
    while (start >= 0
        && (text.at(start).isLetterOrNumber() || text.at(start) == QLatin1Char('_')
            || text.at(start) == QLatin1Char('~')))
        --start;
    return text.mid(start + 1, end - start);
}

/// Every public virtual of `className`, by name.
///
/// Public, because that is what a caller holding a drive can ask and therefore
/// what a decorator has to answer for. A protected virtual is a hook for the
/// class's own implementation -- askWhatIsOffered() is one, reached by the
/// probe() that is forwarded -- and forwarding it would be meaningless.
QSet<QString> publicVirtualsOf(const QString& path, const QString& className)
{
    const QString text = withoutComments(path);
    const qsizetype classAt = text.indexOf(QStringLiteral("class ") + className);
    if (classAt < 0)
        return {};

    // The class body, by bracing, so a later class in the same header is not
    // read as part of this one.
    const qsizetype bodyAt = text.indexOf(QLatin1Char('{'), classAt);
    if (bodyAt < 0)
        return {};
    int depth = 0;
    qsizetype bodyEnd = -1;
    for (qsizetype i = bodyAt; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('{')) {
            ++depth;
        } else if (text.at(i) == QLatin1Char('}') && --depth == 0) {
            bodyEnd = i;
            break;
        }
    }
    if (bodyEnd < 0)
        return {};
    const QString body = text.mid(bodyAt, bodyEnd - bodyAt);

    QSet<QString> names;
    static const QRegularExpression declaration(
        QStringLiteral("\\bvirtual\\b|\\bpublic:|\\bprotected:|\\bprivate:"));
    bool isPublic = false;
    QRegularExpressionMatchIterator it = declaration.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString token = match.captured();
        if (token == QStringLiteral("public:")) {
            isPublic = true;
            continue;
        }
        if (token == QStringLiteral("protected:") || token == QStringLiteral("private:")) {
            isPublic = false;
            continue;
        }
        if (!isPublic)
            continue;

        // From `virtual`, the first bracket opened is the argument list, and the
        // name is what precedes it.
        const qsizetype open = body.indexOf(QLatin1Char('('), match.capturedEnd());
        if (open < 0)
            continue;
        int nested = 0;
        qsizetype close = -1;
        for (qsizetype i = open; i < body.size(); ++i) {
            if (body.at(i) == QLatin1Char('(')) {
                ++nested;
            } else if (body.at(i) == QLatin1Char(')') && --nested == 0) {
                close = i;
                break;
            }
        }
        if (close < 0)
            continue;
        const QString name = methodEndingAt(body, close);
        if (!name.isEmpty() && !name.startsWith(QLatin1Char('~')))
            names.insert(name);
    }
    return names;
}

/// Every method `path` declares with `override`, by name.
QSet<QString> overridesIn(const QString& path)
{
    const QString text = withoutComments(path);
    QSet<QString> names;
    static const QRegularExpression marked(QStringLiteral("\\boverride\\b"));
    QRegularExpressionMatchIterator it = marked.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        // `override` follows the argument list, possibly with const or a
        // reference qualifier between them, so the bracket to match back from is
        // the last one closed before it.
        const qsizetype close = text.lastIndexOf(QLatin1Char(')'), match.capturedStart());
        if (close < 0)
            continue;
        const QString name = methodEndingAt(text, close);
        if (!name.isEmpty())
            names.insert(name);
    }
    return names;
}

} // namespace

/// What a drive answers once something has been wrapped around it.
///
/// Every mount in the application is behind two wrappers -- VersionGuard, then
/// LoggingFileSystem -- and most of the suite holds a bare backend, which is
/// what let four methods go unforwarded for as long as they did. The two claims
/// here are that a wrapper adds nothing to what the drive says, and that a
/// method added to IFileSystem cannot go unforwarded quietly.
class TestWrappedDriveAnswers : public QObject
{
    Q_OBJECT

private slots:
    void theLogWrapperAnswersNothingItself();
    void theVersionGuardAnswersNothingItself();
    void theFaultInjectorAnswersNothingItself();
    void bothWrappersTogetherAnswerNothingThemselves();

    void aMountedDiskKeepsItsNameRulesAndItsCaseFolding();
    void aMountedDriveStillReportsAndDiscardsWhatItIsHolding();

    void everyPublicMethodOfTheInterfaceIsForwardedByEveryDecorator();
};

void TestWrappedDriveAnswers::theLogWrapperAnswersNothingItself()
{
    checkForwards("LoggingFileSystem",
        [](FileSystemPtr inner) { return withLogging(std::move(inner), QStringLiteral("Opinion")); });
}

void TestWrappedDriveAnswers::theVersionGuardAnswersNothingItself()
{
    checkForwards("VersionGuard", [](FileSystemPtr inner) { return withVersionGuard(std::move(inner)); });
}

void TestWrappedDriveAnswers::theFaultInjectorAnswersNothingItself()
{
    // The test fixture is held to the same rule as the shipped wrappers, and for
    // a sharper reason: a suite that injects a fault into a drive and loses its
    // case folding on the way is a suite testing something other than the drive.
    checkForwards("FaultyFileSystem", [](FileSystemPtr inner) {
        return std::static_pointer_cast<IFileSystem>(
            std::make_shared<mole::test::FaultyFileSystem>(std::move(inner)));
    });
}

void TestWrappedDriveAnswers::bothWrappersTogetherAnswerNothingThemselves()
{
    // The arrangement VfsManager::addMount() actually builds, in that order.
    // Each wrapper passing on its own is not the same claim: the outer one is
    // what a caller holds, and it is the outer one that was answering for both.
    checkForwards("the mount as it is really built", [](FileSystemPtr inner) {
        return withLogging(withVersionGuard(std::move(inner)), QStringLiteral("Opinion"));
    });
}

void TestWrappedDriveAnswers::aMountedDiskKeepsItsNameRulesAndItsCaseFolding()
{
    // Through the manager, never on the backend that was handed to it. Asking
    // the bare object is what hid this: LocalFileSystem has always answered
    // correctly, and no caller in the application holds one.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    VfsManager manager;
    Mount mount;
    mount.displayName = QStringLiteral("Disk");
    mount.root = VfsUri(QStringLiteral("file"), QString(), dir.path());
    mount.fileSystem = std::make_shared<LocalFileSystem>();
    QVERIFY(!manager.addMount(mount).isEmpty());

    const FileSystemPtr mounted
        = manager.resolve(VfsUri(QStringLiteral("file"), QString(), dir.path() + QStringLiteral("/x")));
    QVERIFY(mounted != nullptr);
    QVERIFY(mounted.get() != mount.fileSystem.get());

    const LocalFileSystem disk;
    QCOMPARE(mounted->pathCaseSensitivity(), disk.pathCaseSensitivity());

    // The platform's rules, whichever platform this is: the point is that they
    // arrive, and a test naming Windows' answer would only run on Windows.
    const NameRules platform = NameRules::forPlatform();
    const NameRules mountedRules = mounted->nameRules();
    QCOMPARE(mountedRules.forbiddenCharacters, platform.forbiddenCharacters);
    QCOMPARE(mountedRules.refusesControlCharacters, platform.refusesControlCharacters);
    QCOMPARE(mountedRules.refusesTrailingDotOrSpace, platform.refusesTrailingDotOrSpace);
    QCOMPARE(mountedRules.refusesReservedDeviceNames, platform.refusesReservedDeviceNames);
    QCOMPARE(mountedRules.maximumLength, platform.maximumLength);

    // And the one that turns a version uri into a refusal when it is lost.
    QCOMPARE(mounted->understandsVersions(), disk.understandsVersions());
}

void TestWrappedDriveAnswers::aMountedDriveStillReportsAndDiscardsWhatItIsHolding()
{
    // capabilities() was forwarded all along, so the sweep was offered on every
    // mount that advertises ReportsLeftovers and could never find anything on
    // one. The two answers have to arrive through the same object.
    VfsManager manager;
    Mount mount;
    mount.displayName = QStringLiteral("Bucket");
    mount.root = VfsUri(QStringLiteral("opinion"), QString(), QStringLiteral("/"));
    mount.fileSystem = std::make_shared<OpinionatedFileSystem>();
    QVERIFY(!manager.addMount(mount).isEmpty());

    const FileSystemPtr mounted = manager.resolve(VfsUri::fromString(QStringLiteral("opinion:///anything")));
    QVERIFY(mounted != nullptr);
    QVERIFY(mounted->capabilities().testFlag(VfsCapability::ReportsLeftovers));

    const CancelToken cancel;
    const Result<QList<DriveLeftover>> held = mounted->leftovers(std::chrono::hours(24), cancel);
    QVERIFY2(held.ok(), qPrintable(held.error().message));
    QCOMPARE(held.value().size(), 1);
    QCOMPARE(held.value().first().bytes, 4004);

    QVERIFY2(mounted->discardLeftover(held.value().first()).ok(), "the sweep could not discard one");
}

void TestWrappedDriveAnswers::everyPublicMethodOfTheInterfaceIsForwardedByEveryDecorator()
{
    const QString src = QStringLiteral(MOLE_SHELL_SOURCE_DIR);
    const QSet<QString> required
        = publicVirtualsOf(src + QStringLiteral("/core/vfs/IFileSystem.h"), QStringLiteral("IFileSystem"));

    // A scan that found nothing would pass this without reading a line, which is
    // the one way a test over the source fails silently.
    QVERIFY2(required.size() > 15,
        qPrintable(QStringLiteral("only %1 public virtuals found on IFileSystem").arg(required.size())));
    for (const char* expected : { "scheme", "list", "nameRules", "leftovers", "probe" })
        QVERIFY2(required.contains(QLatin1String(expected)), expected);
    QVERIFY2(!required.contains(QStringLiteral("askWhatIsOffered")),
        "a protected hook is not something a decorator can forward");

    const QList<QPair<QString, QString>> decorators {
        { src + QStringLiteral("/core/diagnostics/LoggingFileSystem.h"),
            QStringLiteral("LoggingFileSystem") },
        { src + QStringLiteral("/core/vfs/VersionGuard.h"), QStringLiteral("VersionGuard") },
        { QStringLiteral(MOLE_TEST_SOURCE_DIR) + QStringLiteral("/support/FaultyFileSystem.h"),
            QStringLiteral("FaultyFileSystem") },
    };

    QStringList missing;
    for (const auto& [path, name] : decorators) {
        const QSet<QString> declared = overridesIn(path);
        QVERIFY2(declared.size() > 15,
            qPrintable(QStringLiteral("only %1 overrides found in %2").arg(declared.size()).arg(path)));

        QStringList absent = QStringList(QSet<QString>(required - declared).values());
        absent.sort();
        for (const QString& method : absent)
            missing << name + QStringLiteral("::") + method + QStringLiteral("()");
    }

    // Whoever added the method is the person holding all the context about what
    // the wrapper should do with it, which is why this fails now rather than
    // leaving a plausible default to be found by a user in a year.
    QVERIFY2(missing.isEmpty(),
        qPrintable(QStringLiteral("a decorator over IFileSystem does not forward:\n    %1\n"
                                  "Every public method of the interface has to be passed on, or the "
                                  "drive underneath is never asked -- see MOLE-282.")
                       .arg(missing.join(QStringLiteral("\n    ")))));
}

MOLE_TEST_MAIN(TestWrappedDriveAnswers)
#include "tst_WrappedDriveAnswers.moc"
