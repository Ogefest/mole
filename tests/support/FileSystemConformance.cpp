#include "FileSystemConformance.h"

#include "core/vfs/NameRules.h"
#include "core/vfs/PartialWrite.h"
#include "core/vfs/VersionGuard.h"

#include <QSet>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <thread>

namespace mole::test {
namespace {

    const FileEntry* findEntry(const FileEntryList& entries, const QString& name)
    {
        const auto it = std::find_if(
            entries.begin(), entries.end(), [&name](const FileEntry& e) { return e.name == name; });
        return it == entries.end() ? nullptr : &*it;
    }

} // namespace

void runFileSystemConformance(const ConformanceContext& context)
{
    QVERIFY2(context.fileSystem != nullptr, "conformance context has no backend");
    QVERIFY2(context.seedFile && context.seedDir, "conformance context has no seed helpers");

    IFileSystem& fs = *context.fileSystem;
    const CancelToken noCancel;

    // --- scheme and root -------------------------------------------------
    QCOMPARE(fs.scheme(), context.root.scheme());
    QVERIFY2(fs.capabilities().testFlag(VfsCapability::Read), "every backend must support reading");

    // --- listing an empty root -------------------------------------------
    {
        Result<FileEntryList> listing = fs.list(context.root, noCancel);
        QVERIFY2(listing.ok(), qPrintable(listing.error().message));
        QVERIFY2(listing.value().isEmpty(), "conformance root must start out empty");
    }

    // --- flat listing reports names, kinds and sizes ----------------------
    QVERIFY(context.seedFile(QStringLiteral("alpha.txt"), QByteArray("hello")));
    QVERIFY(context.seedFile(QStringLiteral("beta.log"), QByteArray()));
    QVERIFY(context.seedDir(QStringLiteral("nested")));
    QVERIFY(context.seedFile(QStringLiteral("nested/inner.dat"), QByteArray("12345")));

    {
        Result<FileEntryList> listing = fs.list(context.root, noCancel);
        QVERIFY2(listing.ok(), qPrintable(listing.error().message));
        const FileEntryList& entries = listing.value();
        QCOMPARE(entries.size(), 3);

        const FileEntry* alpha = findEntry(entries, QStringLiteral("alpha.txt"));
        QVERIFY2(alpha, "alpha.txt missing from listing");
        QVERIFY(!alpha->isDir);
        QCOMPARE(alpha->size, 5);
        QCOMPARE(alpha->uri, context.root.child(QStringLiteral("alpha.txt")));
        QCOMPARE(alpha->uri.suffix(), QStringLiteral("txt"));

        const FileEntry* nested = findEntry(entries, QStringLiteral("nested"));
        QVERIFY2(nested, "nested directory missing from listing");
        QVERIFY(nested->isDir);

        // Listing is not recursive.
        QVERIFY2(!findEntry(entries, QStringLiteral("inner.dat")), "list() must return direct children only");
    }

    // --- what the backend claims about case, against what the volume does -
    //
    // `pathCaseSensitivity()` is a constant per backend, and until MOLE-318 nothing
    // had ever held one against the server behind it. `SmbFileSystem` never
    // overrode `IFileSystem`'s default, so it told the rest of Mole that a Samba
    // share behaves like ext4 -- and three places believed it:
    //
    // - the rename guard below, which refused a case-only rename outright, because
    //   on a share the file in the way is the file being renamed;
    // - `TransferTask`, which asks the *target* what it does about case to decide
    //   whether a copy is about to land on something already there. On a share it
    //   judged `Report.pdf` and `report.pdf` to be different destinations, so a
    //   transfer could overwrite a file nobody was warned about -- which is the
    //   failure that check exists to prevent;
    // - `BulkRenameFeature`, which asks it whether two new names collide.
    //
    // Only the first of those had a sighting. The other two follow from the same
    // wrong answer, which is why this is held here rather than beside the rename:
    // one claim, checked once, for every backend at once.
    //
    // `alpha.txt` is on the volume. Whether `ALPHA.TXT` finds it is what the volume
    // does, and what the backend says has to be the same thing.
    {
        const Qt::CaseSensitivity claimed = fs.pathCaseSensitivity();
        const bool otherSpellingFindsIt = fs.stat(context.root.child(QStringLiteral("ALPHA.TXT"))).ok();

        if (otherSpellingFindsIt) {
            QVERIFY2(claimed == Qt::CaseInsensitive,
                "this volume answered a stat for ALPHA.TXT while the file is alpha.txt, so it does "
                "not distinguish case -- and pathCaseSensitivity() says it does. A transfer onto it "
                "will judge Report.pdf and report.pdf to be different destinations and overwrite a "
                "file nobody was warned about. Override pathCaseSensitivity() on this backend.");
        } else {
            QVERIFY2(claimed == Qt::CaseSensitive,
                "this volume did not answer a stat for ALPHA.TXT while the file is alpha.txt, so it "
                "distinguishes case -- and pathCaseSensitivity() says it does not. A case-only "
                "rename will be taken for a no-op, and a bulk rename will refuse names that do not "
                "collide. A volume that disagrees with its platform -- a FAT stick on Linux, a "
                "case-sensitive APFS volume -- is the limitation "
                "LocalFileSystem::pathCaseSensitivity() records in writing, not a fault here.");
        }
    }

    // --- listing a subdirectory ------------------------------------------
    {
        Result<FileEntryList> listing = fs.list(context.root.child(QStringLiteral("nested")), noCancel);
        QVERIFY2(listing.ok(), qPrintable(listing.error().message));
        QCOMPARE(listing.value().size(), 1);
        QCOMPARE(listing.value().first().name, QStringLiteral("inner.dat"));
    }

    // --- stat -------------------------------------------------------------
    {
        Result<FileEntry> stat = fs.stat(context.root.child(QStringLiteral("alpha.txt")));
        QVERIFY2(stat.ok(), qPrintable(stat.error().message));
        QCOMPARE(stat.value().name, QStringLiteral("alpha.txt"));
        QCOMPARE(stat.value().size, 5);
        QVERIFY(!stat.value().isDir);
    }

    // --- error mapping ----------------------------------------------------
    {
        Result<FileEntry> missing = fs.stat(context.root.child(QStringLiteral("does-not-exist")));
        QVERIFY2(!missing.ok(), "stat on a missing path must fail");
        QCOMPARE(missing.error().code, VfsError::NotFound);
        QVERIFY2(!missing.error().message.isEmpty(), "errors must carry a human-readable message");
    }
    {
        Result<FileEntryList> notADir = fs.list(context.root.child(QStringLiteral("alpha.txt")), noCancel);
        QVERIFY2(!notADir.ok(), "listing a regular file must fail");
        QCOMPARE(notADir.error().code, VfsError::NotADirectory);
    }
    {
        Result<FileEntryList> missing = fs.list(context.root.child(QStringLiteral("nope")), noCancel);
        QVERIFY2(!missing.ok(), "listing a missing directory must fail");
        QCOMPARE(missing.error().code, VfsError::NotFound);
    }

    // --- cancellation is observed ----------------------------------------
    //
    // **Every operation, not only the listing.** `list()` was the only method
    // that took a token, so every backend filled the gap with a fresh one --
    // thirty-three sites across four network backends -- and a recursive
    // `remove()` over a large remote tree, a rename of an S3 "directory" (a copy
    // and a delete per key), a whole-file `openRead()` and a staged 64 MiB PUT in
    // `openWrite()`'s close all ran to completion whatever the user did. The task
    // layer could stop only *between* backend calls. See ADR-0096 and MOLE-368.
    {
        CancelToken cancelled;
        cancelled.cancel();
        Result<FileEntryList> listing = fs.list(context.root, cancelled);
        QVERIFY2(!listing.ok(), "a pre-cancelled token must abort the listing");
        QCOMPARE(listing.error().code, VfsError::Cancelled);

        // A read that would fetch the whole file on a remote drive.
        Result<std::unique_ptr<QIODevice>> reading
            = fs.openRead(context.root.child(QStringLiteral("alpha.txt")), -1, cancelled);
        QVERIFY2(!reading.ok(), "a pre-cancelled token must abort the read before it starts");
        QCOMPARE(reading.error().code, VfsError::Cancelled);

        if (context.expectsWriteSupport) {
            // And the two that walk a tree. `nested` is seeded above and its
            // contents are asserted on further down, so this must not delete it:
            // the token is what stops it, and that is the whole assertion.
            const Result<void> removing
                = fs.remove(context.root.child(QStringLiteral("nested")), true, cancelled);
            QVERIFY2(!removing.ok(), "a pre-cancelled token must abort a recursive remove");
            QCOMPARE(removing.error().code, VfsError::Cancelled);

            const Result<void> renaming = fs.rename(context.root.child(QStringLiteral("nested")),
                context.root.child(QStringLiteral("moved-nowhere")), cancelled);
            QVERIFY2(!renaming.ok(), "a pre-cancelled token must abort a rename");
            QCOMPARE(renaming.error().code, VfsError::Cancelled);

            Result<std::unique_ptr<QIODevice>> writing
                = fs.openWrite(context.root.child(QStringLiteral("never-written.bin")), 4, cancelled);
            QVERIFY2(!writing.ok(), "a pre-cancelled token must abort a write before it starts");
            QCOMPARE(writing.error().code, VfsError::Cancelled);
        }
    }

    {
        // Opening a directory as a file. Callers branch on the code, so the
        // requirement is that it says something they can branch on: not ok, and
        // not Unknown, which is the answer that breaks every one of them.
        Result<std::unique_ptr<QIODevice>> asFile = fs.openRead(context.root.child(QStringLiteral("nested")));
        QVERIFY2(!asFile.ok(), "opening a directory as a file must fail");
        QVERIFY2(asFile.error().code != VfsError::Unknown,
            qPrintable(QStringLiteral("a caller cannot act on Unknown: %1").arg(asFile.error().message)));
    }

    // --- reading content --------------------------------------------------
    {
        Result<std::unique_ptr<QIODevice>> device
            = fs.openRead(context.root.child(QStringLiteral("alpha.txt")));
        QVERIFY2(device.ok(), qPrintable(device.error().message));
        QCOMPARE(device.value()->readAll(), QByteArray("hello"));
    }

    // --- reading part of a file -------------------------------------------
    //
    // The span loop that makes a hundred-gigabyte file copyable rests entirely
    // on this: it seeks, reads a stretch, and seeks again. A backend that
    // answers a ranged read with the whole file, or with the right bytes from
    // the wrong offset, produces a copy that is the right length and wrong.
    if (fs.capabilities().testFlag(VfsCapability::RandomAccessRead)) {
        const QByteArray alphabet = QByteArrayLiteral("abcdefghijklmnopqrstuvwxyz");
        QVERIFY(context.seedFile(QStringLiteral("ranged.bin"), alphabet));
        const VfsUri ranged = context.root.child(QStringLiteral("ranged.bin"));

        Result<std::unique_ptr<QIODevice>> device = fs.openRead(ranged, alphabet.size());
        QVERIFY2(device.ok(), qPrintable(device.error().message));
        QIODevice& file = *device.value();

        QCOMPARE(file.read(4), QByteArrayLiteral("abcd"));

        QVERIFY2(file.seek(10), "a backend advertising random access must be able to seek");
        QCOMPARE(file.read(4), QByteArrayLiteral("klmn"));

        // The end, where a read that asks for more than is left must answer with
        // what is left rather than with an error.
        QVERIFY(file.seek(alphabet.size() - 3));
        QCOMPARE(file.read(100), QByteArrayLiteral("xyz"));

        // And past it, where the honest answer is nothing at all.
        QVERIFY(file.seek(alphabet.size()));
        QCOMPARE(file.read(10), QByteArray());
    }

    // --- two things at once -----------------------------------------------
    //
    // Every backend here claims to tolerate being used from more than one
    // thread, because the task layer does exactly that: a scan and a listing
    // run at the same time on one drive. A pool that hands the same connection
    // to both produces answers that are individually plausible and belong to
    // the other caller's question.
    {
        std::atomic<int> wrong { 0 };
        const auto askRepeatedly = [&fs, &context, &wrong, &noCancel] {
            for (int i = 0; i < 10; ++i) {
                const Result<FileEntry> stat = fs.stat(context.root.child(QStringLiteral("alpha.txt")));
                if (!stat.ok() || stat.value().size != 5)
                    ++wrong;
                const Result<FileEntryList> listing
                    = fs.list(context.root.child(QStringLiteral("nested")), noCancel);
                if (!listing.ok() || listing.value().size() != 1)
                    ++wrong;
            }
        };

        std::thread first(askRepeatedly);
        std::thread second(askRepeatedly);
        first.join();
        second.join();
        QCOMPARE(wrong.load(), 0);
    }

    // --- optional: what only this drive can do ---------------------------
    //
    // A drive may offer an action nothing else has -- earlier versions of a
    // file, a link to an object -- and nothing above the backend knows what it
    // is. Both directions are checked, because both go wrong: a drive that
    // offers an action it will not then perform, and a drive that offers none
    // but answers invoke() anyway, which is the one that would let a feature
    // work by accident on the backend it was written against.
    {
        const VfsUri subject = context.root.child(QStringLiteral("alpha.txt"));
        const Result<FileEntry> entry = fs.stat(subject);
        QVERIFY2(entry.ok(), qPrintable(entry.error().message));

        const QString neverOffered = QStringLiteral("org.mole.conformance.never-offered");
        const FileActionList actions = fs.actionsFor(subject, entry.value());

        QSet<QString> ids;
        for (const FileAction& action : actions) {
            QVERIFY2(!action.id.isEmpty(), "a contributed action must carry an id");
            // Namespaced, because two drives loaded at once must not be able to
            // collide, and an id outlives the session that produced it.
            QVERIFY2(action.id.contains(QLatin1Char('.')),
                qPrintable(QStringLiteral("an action id must be namespaced: %1").arg(action.id)));
            QVERIFY2(!action.title.isEmpty(),
                qPrintable(QStringLiteral("%1 has nothing to show in a menu").arg(action.id)));
            QVERIFY2(
                !ids.contains(action.id), qPrintable(QStringLiteral("%1 was offered twice").arg(action.id)));
            ids.insert(action.id);

            if (!action.enabled)
                continue;

            const Result<FileActionOutcome> outcome = fs.invoke(action.id, subject, noCancel);
            QVERIFY2(outcome.ok(),
                qPrintable(QStringLiteral("%1 was offered and then refused: %2")
                               .arg(action.id, outcome.error().message)));
            // The two kinds are the whole vocabulary the interface has. An
            // outcome carrying neither is one nothing can be shown for, and a
            // drive that returns one has said it did something it did not do.
            QVERIFY2(outcome.value().isValid(),
                qPrintable(QStringLiteral("%1 answered with neither text nor uris").arg(action.id)));
            // What it said it would answer with. A caller that only wants one of
            // the two kinds -- the preview wants uris and has no use for a link
            // -- picks by this rather than by performing every action to find
            // out, and performing one has effects.
            QVERIFY2(outcome.value().kind == action.answers,
                qPrintable(
                    QStringLiteral("%1 did not answer with the kind it said it would").arg(action.id)));
            for (const VfsUri& alternate : outcome.value().uris) {
                QVERIFY2(alternate.isValid(),
                    qPrintable(QStringLiteral("%1 returned a uri nothing can open").arg(action.id)));
            }
        }

        // Whether it offered anything or not: an id this drive did not hand out
        // is refused, and refused in the one way a caller can branch on.
        const Result<FileActionOutcome> unknown = fs.invoke(neverOffered, subject, noCancel);
        QVERIFY2(!unknown.ok(),
            qPrintable(actions.isEmpty()
                    ? QStringLiteral("a drive offering no action must not answer invoke()")
                    : QStringLiteral("an id this drive never offered must not be performed")));
        QCOMPARE(unknown.error().code, VfsError::NotSupported);
    }

    // --- a uri that names an earlier version of a file --------------------
    //
    // Read-only, and refused by every drive that does not know what a version
    // is. The refusal is the point of the case: a backend that ignored a token
    // it did not recognise would answer with the *current* file while the window
    // says it is showing an earlier one -- a silent wrong answer on the one
    // screen whose entire purpose is to say which version you are looking at.
    {
        const VfsUri current = context.root.child(QStringLiteral("alpha.txt"));
        const VfsUri versioned = current.withVersion(QStringLiteral("an-earlier-one"));

        QVERIFY2(versioned.hasVersion(), "withVersion() must produce a uri that carries one");
        QVERIFY2(!(versioned == current), "a version is part of what makes a uri that uri");
        // Written down and read back: a bookmark and a restored session are both
        // exactly this, and a uri that does not survive it is a dead bookmark.
        QCOMPARE(VfsUri::fromString(versioned.toString()), versioned);
        QCOMPARE(VfsUri::fromString(current.toString()), current);

        if (!fs.understandsVersions()) {
            // Reached through the guard, which is how every drive in Mole is
            // reached: VfsManager puts one on each mount the way it puts the log
            // wrapper on. A backend that implements versions is handed the uri
            // unchanged and answers for itself.
            const FileSystemPtr guarded = withVersionGuard(context.fileSystem);

            const Result<FileEntry> stat = guarded->stat(versioned);
            QVERIFY2(!stat.ok(), "a drive that does not do versions must refuse one, not answer");
            QCOMPARE(stat.error().code, VfsError::NotSupported);

            const Result<std::unique_ptr<QIODevice>> read = guarded->openRead(versioned);
            QVERIFY2(!read.ok(), "and must not hand over the current file's bytes for one");
            QCOMPARE(read.error().code, VfsError::NotSupported);

            const Result<FileEntryList> listing = guarded->list(versioned, noCancel);
            QVERIFY2(!listing.ok(), "nor list one");
            QCOMPARE(listing.error().code, VfsError::NotSupported);

            // And the same drive goes on answering about the file as it is.
            QVERIFY2(guarded->stat(current).ok(), "the current file must still be reachable");
        }
    }

    // --- what the flags say, against what the drive does ------------------
    //
    // **The write half is gated on a test-side flag**, so a backend advertising
    // Write while `expectsWriteSupport` is false -- or the reverse -- passed the
    // whole suite. The two have to agree before either means anything, and each
    // flag has to agree with the answer the operation actually gives: a drive
    // that advertises Delete and answers NotSupported is a menu entry that does
    // nothing. See MOLE-401.
    {
        const VfsCapabilities flags = fs.capabilities();
        QCOMPARE(flags.testFlag(VfsCapability::Write), context.expectsWriteSupport);

        struct Claim
        {
            VfsCapability flag;
            const char* name;
            std::function<VfsError()> attempt;
        };

        const VfsUri victim = context.root.child(QStringLiteral("flags-check.bin"));
        const QList<Claim> claims {
            { VfsCapability::Write, "Write",
                [&] {
                    Result<std::unique_ptr<QIODevice>> opened = fs.openWrite(victim, 4, noCancel);
                    if (!opened.ok())
                        return opened.error();
                    opened.value()->write(QByteArray("abcd"));
                    // Through closeAndReport, because on a staging backend the
                    // bare close cannot fail and a refusal arrives there rather
                    // than from openWrite.
                    const Result<void> landed = closeAndReport(*opened.value());
                    opened.value().reset();
                    if (landed.ok())
                        fs.remove(victim, false, noCancel);
                    return landed.ok() ? VfsError {} : landed.error();
                } },
            { VfsCapability::MakeDirectory, "MakeDirectory",
                [&] {
                    const VfsUri folder = context.root.child(QStringLiteral("flags-check-dir"));
                    const Result<void> made = fs.makeDirectory(folder);
                    if (made.ok())
                        fs.remove(folder, true, noCancel);
                    return made.ok() ? VfsError {} : made.error();
                } },
            { VfsCapability::Delete, "Delete",
                [&] {
                    // Of something that is not there: what is being asked is
                    // whether the operation exists, and NotFound is an answer
                    // only a drive that can delete gives.
                    const Result<void> gone
                        = fs.remove(context.root.child(QStringLiteral("no-such-thing")), false, noCancel);
                    return gone.ok() ? VfsError {} : gone.error();
                } },
            { VfsCapability::Rename, "Rename",
                [&] {
                    const Result<void> moved = fs.rename(context.root.child(QStringLiteral("no-such-thing")),
                        context.root.child(QStringLiteral("nor-this")), noCancel);
                    return moved.ok() ? VfsError {} : moved.error();
                } },
        };

        for (const Claim& claim : claims) {
            const VfsError answer = claim.attempt();
            if (flags.testFlag(claim.flag)) {
                QVERIFY2(answer.code != VfsError::NotSupported,
                    qPrintable(QStringLiteral("this drive advertises %1 and then refuses it: %2")
                                   .arg(QLatin1String(claim.name), answer.message)));
            } else {
                QVERIFY2(answer.code == VfsError::NotSupported,
                    qPrintable(QStringLiteral("this drive does not advertise %1 and answered %2 "
                                              "instead of NotSupported")
                                   .arg(QLatin1String(claim.name))
                                   .arg(int(answer.code))));
            }
        }
    }

    // --- how much room, when the drive says it knows ----------------------
    {
        const Result<SpaceInfo> room = fs.space(context.root);
        if (fs.capabilities().testFlag(VfsCapability::ReportsSpace)) {
            QVERIFY2(room.ok(),
                qPrintable(QStringLiteral("this drive advertises ReportsSpace and refused: %1")
                               .arg(room.error().message)));
            QVERIFY2(room.value().totalBytes > 0, "a drive that reports space has to have some");
            QVERIFY2(
                room.value().freeBytes <= room.value().totalBytes, "more room free than there is altogether");
        } else {
            QVERIFY2(!room.ok(), "a drive without ReportsSpace must refuse, not invent");
            QCOMPARE(room.error().code, VfsError::NotSupported);
        }
    }

    // --- what the drive says it will not accept as a name -----------------
    //
    // `nameRules()` is what the rename dialog and every copy check against, and
    // nothing held a drive to *its own* rules: a backend that refuses a character
    // and does not say so, or says so and accepts it anyway, passed. The second
    // is the one that matters -- the check happens above the backend, so a rule
    // nobody enforces is a name that reaches the wire and comes back as a
    // protocol error nobody can act on. See MOLE-401 and ADR-0070.
    {
        const NameRules rules = fs.nameRules();
        QString refused;
        if (!rules.forbiddenCharacters.isEmpty())
            refused = QStringLiteral("bad%1name.txt").arg(rules.forbiddenCharacters.at(0));
        else if (rules.refusesTrailingDotOrSpace)
            refused = QStringLiteral("trailing.");
        else if (rules.refusesReservedDeviceNames)
            refused = QStringLiteral("nul.txt");
        else if (rules.maximumLength > 0)
            refused = QString(rules.maximumLength + 1, QLatin1Char('n'));

        if (refused.isEmpty()) {
            // Nothing to offer: a drive whose rules refuse nothing has nothing to
            // be held to here, and that is an answer rather than a gap.
            QVERIFY2(!checkName(QStringLiteral("ordinary.txt"), rules).isRejected(),
                "a drive that refuses nothing must accept an ordinary name");
        } else {
            QVERIFY2(checkName(refused, rules).isRejected(),
                qPrintable(
                    QStringLiteral("the fixture name %1 is not actually refused by the rules").arg(refused)));
            if (context.expectsWriteSupport) {
                Result<std::unique_ptr<QIODevice>> opened
                    = fs.openWrite(context.root.child(refused), 4, noCancel);
                if (opened.ok()) {
                    opened.value()->write(QByteArray("abcd"));
                    // Collected rather than closed: a staging backend refuses on
                    // the way out, because the working name is one it can open
                    // and the rename into place is what the volume rejects.
                    const Result<void> landed = closeAndReport(*opened.value());
                    opened.value().reset();
                    if (landed.ok()) {
                        // A drive that took it has to have *kept* it under that
                        // name, or the rules are wrong in the other direction: a
                        // silent rewrite is the Windows trailing-dot fault
                        // ADR-0070 is about.
                        const Result<FileEntry> found = fs.stat(context.root.child(refused));
                        QVERIFY2(found.ok(),
                            qPrintable(QStringLiteral("this drive accepted %1, which its own nameRules() "
                                                      "refuse, and did not keep it under that name")
                                           .arg(refused)));
                        fs.remove(context.root.child(refused), false, noCancel);
                    } else {
                        QVERIFY2(!fs.stat(context.root.child(refused)).ok(),
                            qPrintable(QStringLiteral("this drive refused %1 and left it behind anyway")
                                           .arg(refused)));
                    }
                }
            }
        }
    }

    // --- what the drive has left behind, and letting go of it -------------
    {
        const Result<QList<DriveLeftover>> left = fs.leftovers(std::chrono::seconds(0), noCancel);
        if (!left.ok()) {
            QCOMPARE(left.error().code, VfsError::NotSupported);
        } else {
            for (const DriveLeftover& leftover : left.value()) {
                QVERIFY2(!leftover.handle.isEmpty(), "a leftover nothing can name cannot be discarded");
                QVERIFY2(
                    !leftover.what.isEmpty(), "a leftover with nothing to show cannot be offered to anybody");
            }
            // One this drive never issued. The sweep hands back what it was given,
            // so a drive that acts on an id it did not issue would act on
            // another drive's.
            DriveLeftover invented;
            invented.handle = QStringLiteral("org.mole.conformance.not-a-leftover");
            invented.what = QStringLiteral("invented by the conformance suite");
            const Result<void> discarded = fs.discardLeftover(invented);
            QVERIFY2(!discarded.ok(), "a drive discarded a leftover it never issued");
        }
    }

    // --- which entries have something to offer ----------------------------
    {
        const Result<QStringList> withActions = fs.entriesWithActions(context.root, noCancel);
        if (!withActions.ok()) {
            QCOMPARE(withActions.error().code, VfsError::NotSupported);
        } else {
            for (const QString& name : withActions.value()) {
                QVERIFY2(!name.isEmpty(), "an entry with actions has to be named");
                const Result<FileEntry> entry = fs.stat(context.root.child(name));
                QVERIFY2(entry.ok(),
                    qPrintable(QStringLiteral("%1 was said to have actions and does not exist").arg(name)));
                QVERIFY2(!fs.actionsFor(context.root.child(name), entry.value()).isEmpty(),
                    qPrintable(QStringLiteral("%1 was said to have actions and has none").arg(name)));
            }
        }
    }

    // --- what the drive offers, and asking it again -----------------------
    //
    // ADR-0076's three-state answer was held only against a test double. A real
    // backend's offers() has to be self-consistent -- an offer with nothing in it
    // is a menu entry with no label -- and probe() has to be safe to call and to
    // leave the drive answering.
    {
        const DriveOffers before = fs.offers();
        fs.probe(context.root, noCancel);
        const DriveOffers after = fs.offers();

        // Three states, and the two that are not `Unasked` have to be told apart:
        // a drive that could not answer is absent for a reason worth reporting,
        // where an empty answer is a drive with nothing to offer. ADR-0076.
        QVERIFY2(before.state == DriveOffers::State::Unasked || before.state == DriveOffers::State::Answered
                || before.state == DriveOffers::State::Failed,
            "a drive's offers are in one of three states and this is none of them");
        for (const DriveOffers* offers : { &before, &after }) {
            if (offers->state != DriveOffers::State::Answered)
                QVERIFY2(offers->ids.isEmpty(), "a drive that has not answered has offered nothing");
            for (const QString& id : offers->ids) {
                QVERIFY2(!id.isEmpty(), "an offered action must carry an id");
                QVERIFY2(id.contains(QLatin1Char('.')),
                    qPrintable(QStringLiteral("an offered id must be namespaced: %1").arg(id)));
            }
        }

        // And every id the drive offers here is one it will hand out for a node
        // that has it: an offer nothing can act on is a menu that disappoints.
        if (after.state == DriveOffers::State::Answered && !after.ids.isEmpty()) {
            const Result<FileEntry> subject = fs.stat(context.root.child(QStringLiteral("alpha.txt")));
            QVERIFY2(subject.ok(), qPrintable(subject.error().message));
            const FileActionList narrowed
                = fs.actionsFor(context.root.child(QStringLiteral("alpha.txt")), subject.value());
            for (const FileAction& action : narrowed) {
                QVERIFY2(after.ids.contains(action.id),
                    qPrintable(
                        QStringLiteral("%1 was handed out for a file and is in no offer").arg(action.id)));
            }
        }

        // And the drive still works afterwards, which is the half a probe can
        // break: it talks to the server.
        const Result<FileEntryList> afterProbe = fs.list(context.root, noCancel);
        QVERIFY2(afterProbe.ok(),
            qPrintable(QStringLiteral("this drive stopped listing after probe(): %1")
                           .arg(afterProbe.error().message)));
    }

    // --- what this drive can be asked to look for -------------------------
    //
    // NativeSearch was unchecked, so the first backend to advertise it would
    // arrive unchecked. What is asked here is the pair: a drive that advertises
    // it has to answer, and one that does not has to say NotSupported rather
    // than answer emptily -- an empty answer reads as "nothing matches", and a
    // search that silently finds nothing is worse than one that says it cannot.
    {
        const Result<FileEntryList> found = fs.search(context.root, QStringLiteral("alpha*"), noCancel);
        if (fs.capabilities().testFlag(VfsCapability::NativeSearch)) {
            QVERIFY2(found.ok(),
                qPrintable(QStringLiteral("this drive advertises NativeSearch and refused: %1")
                               .arg(found.error().message)));
            for (const FileEntry& hit : found.value()) {
                QVERIFY2(!hit.name.isEmpty(), "a search hit has to be named");
                QVERIFY2(hit.uri.path().startsWith(context.root.path()),
                    qPrintable(
                        QStringLiteral("%1 is not under where the search was asked").arg(hit.uri.path())));
            }
        } else {
            QVERIFY2(!found.ok(), "a drive without NativeSearch must refuse rather than find nothing");
            QCOMPARE(found.error().code, VfsError::NotSupported);
        }
    }

    // --- a token cancelled while the drive is working ---------------------
    //
    // One per operation rather than one for listings: a drive polls its token in
    // as many places as it has loops, and the one place it forgot is the one a
    // user waits out. Each is arranged by the fixture or reported as not run --
    // never quietly passed, which is what an unarrangeable case would otherwise
    // do. See MOLE-401.
    if (context.whileHeldPartWay) {
        struct HeldCall
        {
            const char* what;
            bool needsWriting;
            std::function<VfsError(const CancelToken&)> attempt;
        };

        const VfsUri alpha = context.root.child(QStringLiteral("alpha.txt"));
        const VfsUri written = context.root.child(QStringLiteral("cancel-part-way.bin"));

        // Made before the operations that need something to work on, so a
        // cancelled remove or rename is a cancelled remove or rename rather than
        // a NotFound that looks like one.
        if (context.expectsWriteSupport) {
            Result<std::unique_ptr<QIODevice>> seeded = fs.openWrite(written, 4, noCancel);
            if (seeded.ok()) {
                seeded.value()->write(QByteArray("abcd"));
                closeAndReport(*seeded.value());
                seeded.value().reset();
            }
        }

        QString anAction;
        {
            const Result<FileEntry> subject = fs.stat(alpha);
            if (subject.ok()) {
                const FileActionList offered = fs.actionsFor(alpha, subject.value());
                if (!offered.isEmpty())
                    anAction = offered.first().id;
            }
        }

        QList<HeldCall> calls {
            { "list", false,
                [&](const CancelToken& cancel) {
                    const Result<FileEntryList> listing = fs.list(context.root, cancel);
                    return listing.ok() ? VfsError {} : listing.error();
                } },
            { "openRead", false,
                [&](const CancelToken& cancel) {
                    Result<std::unique_ptr<QIODevice>> opened = fs.openRead(alpha, -1, cancel);
                    return opened.ok() ? VfsError {} : opened.error();
                } },
            { "openWrite", true,
                [&](const CancelToken& cancel) {
                    Result<std::unique_ptr<QIODevice>> opened = fs.openWrite(
                        context.root.child(QStringLiteral("cancel-open-write.bin")), 4, cancel);
                    if (opened.ok()) {
                        closeAndReport(*opened.value());
                        opened.value().reset();
                        fs.remove(
                            context.root.child(QStringLiteral("cancel-open-write.bin")), false, noCancel);
                    }
                    return opened.ok() ? VfsError {} : opened.error();
                } },
            { "remove", true,
                [&](const CancelToken& cancel) {
                    const Result<void> gone = fs.remove(written, false, cancel);
                    return gone.ok() ? VfsError {} : gone.error();
                } },
            { "rename", true,
                [&](const CancelToken& cancel) {
                    const Result<void> moved = fs.rename(
                        written, context.root.child(QStringLiteral("cancel-renamed.bin")), cancel);
                    if (moved.ok())
                        fs.rename(
                            context.root.child(QStringLiteral("cancel-renamed.bin")), written, noCancel);
                    return moved.ok() ? VfsError {} : moved.error();
                } },
        };

        if (!anAction.isEmpty()) {
            calls.append({ "invoke", false, [&](const CancelToken& cancel) {
                              const Result<FileActionOutcome> outcome = fs.invoke(anAction, alpha, cancel);
                              return outcome.ok() ? VfsError {} : outcome.error();
                          } });
        }

        // probe() is not in the list, and not because it cannot be held. It
        // answers into offers() rather than to its caller, and the section above
        // has already probed successfully -- so a cancelled probe has nothing
        // observable left to assert here. Where its token is honoured is
        // askWhatIsOffered(), which tst_OfferingFileSystem holds directly.
        QStringList notArranged;
        for (const HeldCall& call : calls) {
            if (call.needsWriting && !context.expectsWriteSupport)
                continue;
            std::atomic_bool noticed { false };
            CancelToken token;
            const bool arranged = context.whileHeldPartWay(
                call.what,
                [&](const CancelToken& cancel) {
                    const VfsError answer = call.attempt(cancel);
                    noticed.store(answer.code == VfsError::Cancelled);
                },
                [&token] { token.cancel(); });
            if (!arranged) {
                notArranged.append(QLatin1String(call.what));
                continue;
            }
            QVERIFY2(noticed.load(),
                qPrintable(QStringLiteral("a token cancelled while %1 was running was not noticed: the "
                                          "operation ran to completion, which is what the task layer "
                                          "cannot undo")
                               .arg(QLatin1String(call.what))));
        }
        if (!notArranged.isEmpty()) {
            // Said out loud rather than left silent: a case nobody can arrange
            // reads exactly like a case that passed.
            qInfo("this fixture cannot hold these calls part way, so their mid-flight cancel was not "
                  "checked: %s",
                qPrintable(notArranged.join(QStringLiteral(", "))));
        }
    }

    if (!context.expectsWriteSupport)
        return;

    // --- writing content --------------------------------------------------
    //
    // The suite used to seed every fixture out of band and never once write
    // through the backend, so the whole write path was uncovered -- which is how
    // a WebDAV backend that could not write a file larger than a few hundred
    // bytes stayed green for months. Two sizes, because that fault was invisible
    // at one of them: a body small enough for the transport to hold a copy of
    // behaves differently from one that is not.
    {
        const VfsUri written = context.root.child(QStringLiteral("written.bin"));
        const QByteArray small = QByteArrayLiteral("written through the backend under test");

        Result<std::unique_ptr<QIODevice>> out = fs.openWrite(written, small.size());
        QVERIFY2(out.ok(), qPrintable(out.error().message));
        QCOMPARE(out.value()->write(small), static_cast<qint64>(small.size()));
        Result<void> closed = closeAndReport(*out.value());
        QVERIFY2(closed.ok(), qPrintable(closed.error().message));

        Result<FileEntry> stat = fs.stat(written);
        QVERIFY2(stat.ok(), qPrintable(stat.error().message));
        QCOMPARE(stat.value().size, static_cast<qint64>(small.size()));

        Result<std::unique_ptr<QIODevice>> back = fs.openRead(written, small.size());
        QVERIFY2(back.ok(), qPrintable(back.error().message));
        QCOMPARE(back.value()->readAll(), small);
        // Let go before writing over it. Reading and then overwriting while still
        // holding the read open is not what this is about, and a share refuses it:
        // Windows semantics do not let a file be replaced while somebody has it
        // open, where POSIX quietly allows it. Every backend here reads the file
        // and then writes it; none of them needs to do both at once.
        back.value().reset();

        // Again, over the same name. An overwrite is ordinary -- it is what a
        // re-run of a failed copy does -- and it must neither be refused nor
        // leave the two versions mixed.
        QByteArray large(64 * 1024, Qt::Uninitialized);
        for (int i = 0; i < large.size(); ++i)
            large[i] = static_cast<char>((i * 37 + (i >> 5)) & 0xff);

        out = fs.openWrite(written, large.size());
        QVERIFY2(out.ok(), qPrintable(out.error().message));
        QCOMPARE(out.value()->write(large), static_cast<qint64>(large.size()));
        closed = closeAndReport(*out.value());
        QVERIFY2(closed.ok(), qPrintable(closed.error().message));

        back = fs.openRead(written, large.size());
        QVERIFY2(back.ok(), qPrintable(back.error().message));
        QCOMPARE(back.value()->readAll(), large);

        // Let go of the read before removing. Holding it open was incidental --
        // this step is about removing what was written -- and on SMB it is not
        // incidental at all: a share refuses to unlink a file somebody has open,
        // which is Windows semantics rather than a fault, and every POSIX backend
        // allowed it only because POSIX does.
        back.value().reset();

        QVERIFY2(fs.remove(written, false).ok(), "removing what this suite wrote must succeed");
    }

    // --- a write that is abandoned rather than closed ----------------------
    //
    // A cancelled copy, or one that failed part way through, destroys its write
    // stream without closing it. Nothing may appear under the name it was aiming
    // at: half a file under the name somebody asked for is indistinguishable
    // from a file that is simply that size, and it is what a move would then
    // delete the original for. The working name is not litter to be left either
    // -- what a live process abandoned, it can clean up. See ADR-0021.
    {
        const VfsUri abandoned = context.root.child(QStringLiteral("abandoned.bin"));
        {
            Result<std::unique_ptr<QIODevice>> out = fs.openWrite(abandoned, 4096);
            QVERIFY2(out.ok(), qPrintable(out.error().message));
            QCOMPARE(out.value()->write(QByteArray(4096, 'p')), qint64(4096));
        }

        QVERIFY2(!fs.stat(abandoned).ok(), "an abandoned write was put in place as if it had finished");

        Result<FileEntryList> after = fs.list(context.root, noCancel);
        QVERIFY2(after.ok(), qPrintable(after.error().message));
        for (const FileEntry& entry : after.value())
            QVERIFY2(!isPartialWrite(entry.name),
                qPrintable(QStringLiteral("an abandoned write left %1 "
                                          "behind")
                               .arg(entry.name)));
    }

    // --- a destination that appeared while the write was in flight ---------
    //
    // Not the same thing as an overwrite, and the difference is the whole of
    // ADR-0020: a destination that was already there when the write began is
    // what the write was for, while one that turned up while the bytes were
    // going over the wire is somebody else's file. Two windows copying the same
    // name onto one share is all it takes, and the second upload to finish
    // destroyed the first. The commit is refused, what arrived is left exactly
    // as it is, and the working name is not left behind either.
    //
    // Skipped for a backend that puts bytes at the destination as it goes; see
    // stagesWrites, which says why that is a fact about the protocol rather than
    // a case nobody got round to.
    if (context.stagesWrites && fs.capabilities().testFlag(VfsCapability::Rename)) {
        const VfsUri contested = context.root.child(QStringLiteral("contested.bin"));
        Result<std::unique_ptr<QIODevice>> out = fs.openWrite(contested, 64);
        QVERIFY2(out.ok(), qPrintable(out.error().message));
        QCOMPARE(out.value()->write(QByteArray(64, 'm')), qint64(64));

        // Somebody else's file, arriving while that write is still open. Seeded
        // under another name and renamed on, because a backend with no second
        // client to seed with -- NFS has none, libnfs being the only one this
        // process has -- seeds through itself, and would write straight over
        // this write's own working name.
        const QByteArray theirs = QByteArrayLiteral("what somebody else put there");
        QVERIFY2(context.seedFile(QStringLiteral("theirs.bin"), theirs), "seeding failed");
        QVERIFY2(fs.rename(context.root.child(QStringLiteral("theirs.bin")), contested).ok(),
            "renaming onto a free name must succeed");

        const Result<void> closed = closeAndReport(*out.value());
        QVERIFY2(!closed.ok(), "a file that appeared during the write was destroyed by it");
        QCOMPARE(closed.error().code, VfsError::AlreadyExists);

        Result<std::unique_ptr<QIODevice>> back = fs.openRead(contested);
        QVERIFY2(back.ok(), qPrintable(back.error().message));
        QCOMPARE(back.value()->readAll(), theirs);
        // Let go before removing, for the reason the write case above gives: a
        // share will not unlink a file somebody has open.
        back.value().reset();

        Result<FileEntryList> after = fs.list(context.root, noCancel);
        QVERIFY2(after.ok(), qPrintable(after.error().message));
        for (const FileEntry& entry : after.value()) {
            QVERIFY2(!isPartialWrite(entry.name),
                qPrintable(QStringLiteral("a refused commit left %1 behind").arg(entry.name)));
        }

        QVERIFY2(fs.remove(contested, false).ok(), "removing what this suite renamed must succeed");
    }

    // --- links, on a drive that says it holds them -------------------------
    //
    // Gated on the capability rather than on a flag in the context: a drive that
    // advertises Symlink is promising exactly this, and one that does not is
    // promising nothing. What a copy needs from it is the round trip -- the text
    // goes in, the same text comes back, and a listing calls the entry a link --
    // because a copy of a link is a copy of that text and nothing else. Above
    // all the target is stored as given: a drive that resolved a relative link
    // would turn a relocatable tree into one pinned to where it was copied from.
    // See ADR-0092.
    if (context.expectsWriteSupport && fs.capabilities().testFlag(VfsCapability::Symlink)) {
        const VfsUri link = context.root.child(QStringLiteral("pointer"));
        const QString relative = QStringLiteral("neighbour.bin");
        QVERIFY2(fs.makeLink(link, relative).ok(), "a drive advertising Symlink must make one");

        const Result<QString> points = fs.readLink(link);
        QVERIFY2(points.ok(), qPrintable(points.error().message));
        QCOMPARE(points.value(), relative);

        const Result<FileEntry> stat = fs.stat(link);
        QVERIFY2(stat.ok(), qPrintable(stat.error().message));
        QVERIFY2(stat.value().isSymlink, "a link has to be reported as one");

        // The name is taken, whatever it points at -- the same answer
        // makeDirectory() gives, and the reason a copy over a link has to remove
        // it first rather than write through it.
        QCOMPARE(fs.makeLink(link, relative).error().code, VfsError::AlreadyExists);

        // And a name that is not a link says so, rather than answering with
        // something a caller could mistake for a target.
        QVERIFY2(
            context.seedFile(QStringLiteral("ordinary.bin"), QByteArrayLiteral("bytes")), "seeding failed");
        QCOMPARE(
            fs.readLink(context.root.child(QStringLiteral("ordinary.bin"))).error().code, VfsError::NotALink);

        // Removing a link removes the name and never what it points at, which is
        // what makes a move of one safe.
        QVERIFY2(fs.remove(link, false).ok(), "removing a link must succeed");
        QVERIFY(!fs.stat(link).ok());
    }

    // --- a directory that cannot be listed ---------------------------------
    //
    // "I could not read it" and "there is nothing in it" are the same sentence
    // to everything above this layer, and one of them is a lie that costs data:
    // a mirror plans the destination folder empty on the strength of it, a
    // folder-size report says zero, and a move copies an empty directory and
    // then removes the source. An error is the only answer that cannot be
    // mistaken for a fact about the contents.
    if (context.whileUnlistable) {
        const VfsUri locked = context.root.child(QStringLiteral("locked-away"));
        QVERIFY2(fs.makeDirectory(locked).ok(), "makeDirectory must succeed on a free name");
        QVERIFY2(context.seedFile(QStringLiteral("locked-away/inside.txt"), QByteArrayLiteral("hidden")),
            "seeding failed");

        const bool ran = context.whileUnlistable(QStringLiteral("locked-away"), [&] {
            const Result<FileEntryList> listing = fs.list(locked, noCancel);
            QVERIFY2(!listing.ok(), "a directory this account cannot read must not list as empty");
            QCOMPARE(listing.error().code, VfsError::AccessDenied);
        });
        if (!ran)
            qInfo("skipped: this account can list a directory it has no permissions on");

        QVERIFY(fs.remove(locked, true).ok());
    }

    // --- a directory with nothing in it -----------------------------------
    //
    // Worth its own case because on S3 a directory is not a thing: an empty one
    // exists only as a zero-byte key, and a backend that forgets to make it
    // reports a directory the user just created as missing.
    {
        const VfsUri empty = context.root.child(QStringLiteral("empty-one"));
        QVERIFY2(fs.makeDirectory(empty).ok(), "makeDirectory must succeed on a free name");

        Result<FileEntry> stat = fs.stat(empty);
        QVERIFY2(stat.ok(), "a directory that was just created must exist");
        QVERIFY2(stat.value().isDir, "and must be a directory");

        Result<FileEntryList> listing = fs.list(empty, noCancel);
        QVERIFY2(listing.ok(), qPrintable(listing.error().message));
        QVERIFY2(listing.value().isEmpty(), "an empty directory lists as empty, not as an error");

        QVERIFY(fs.remove(empty, false).ok());
    }

    // --- mkdir ------------------------------------------------------------
    QVERIFY(fs.capabilities().testFlag(VfsCapability::MakeDirectory));
    {
        const VfsUri fresh = context.root.child(QStringLiteral("created"));
        QVERIFY2(fs.makeDirectory(fresh).ok(), "makeDirectory must succeed on a free name");

        Result<FileEntry> stat = fs.stat(fresh);
        QVERIFY(stat.ok());
        QVERIFY(stat.value().isDir);

        Result<void> again = fs.makeDirectory(fresh);
        QVERIFY2(!again.ok(), "makeDirectory must refuse an existing path");
        QCOMPARE(again.error().code, VfsError::AlreadyExists);
    }

    // --- a write onto a name that is a directory ---------------------------
    //
    // Three backends had three answers, and one of them destroyed the folder.
    // The local disk saw QFileInfo::exists() say true, took that as "the caller
    // is overwriting a file it knew about", and at close() removed the target
    // -- which succeeds for an *empty* directory -- and renamed the file into
    // its place. A non-empty one failed with NotEmpty and a message about
    // rmdir, which says nothing about the real cause. The in-memory drive
    // refused up front with IsADirectory. Nothing here had ever asked, so the
    // disagreement stood.
    //
    // Refusing is the only answer that is safe on every drive: a folder is not
    // an old version of a file, and there is nothing to weigh up. See MOLE-336.
    {
        const VfsUri standing = context.root.child(QStringLiteral("a-folder-not-a-file"));
        QVERIFY2(fs.makeDirectory(standing).ok(), "makeDirectory must succeed on a free name");
        QVERIFY2(context.seedFile(QStringLiteral("a-folder-not-a-file/inside.txt"),
                     QByteArrayLiteral("the folder is not empty")),
            "seeding failed");

        Result<std::unique_ptr<QIODevice>> writer = fs.openWrite(standing, 4);
        QVERIFY2(!writer.ok(), "openWrite onto a directory must be refused");
        QCOMPARE(writer.error().code, VfsError::IsADirectory);

        const Result<FileEntry> after = fs.stat(standing);
        QVERIFY2(after.ok(), "the directory must still be there");
        QVERIFY2(after.value().isDir, "and must still be a directory");
        QVERIFY2(
            fs.stat(standing.child(QStringLiteral("inside.txt"))).ok(), "and must still hold what it held");

        // An empty one is the dangerous half: it is the one a non-recursive
        // remove would have taken away without complaining.
        const VfsUri hollow = context.root.child(QStringLiteral("an-empty-folder"));
        QVERIFY2(fs.makeDirectory(hollow).ok(), "makeDirectory must succeed on a free name");
        Result<std::unique_ptr<QIODevice>> onto = fs.openWrite(hollow, 4);
        QVERIFY2(!onto.ok(), "openWrite onto an empty directory must be refused too");
        QCOMPARE(onto.error().code, VfsError::IsADirectory);
        QVERIFY2(fs.stat(hollow).value().isDir, "the empty directory must still be a directory");

        QVERIFY(fs.remove(hollow, false).ok());
        QVERIFY(fs.remove(standing, true).ok());
    }

    // --- rename -----------------------------------------------------------
    {
        const VfsUri from = context.root.child(QStringLiteral("beta.log"));
        const VfsUri to = context.root.child(QStringLiteral("beta-renamed.log"));
        QVERIFY2(fs.rename(from, to).ok(), "rename must succeed");
        QVERIFY2(!fs.stat(from).ok(), "the old name must be gone after a rename");
        QVERIFY2(fs.stat(to).ok(), "the new name must exist after a rename");

        Result<void> clash = fs.rename(to, context.root.child(QStringLiteral("alpha.txt")));
        QVERIFY2(!clash.ok(), "rename onto an existing path must fail");
        QCOMPARE(clash.error().code, VfsError::AlreadyExists);
    }

    // --- rename that only changes case -------------------------------------
    //
    // Every backend has to allow this, and the ones it is hard for are the
    // volumes that do not distinguish case: there the file being renamed is the
    // file the guard finds in the way, so "already exists" refused it and there
    // was no way to make it happen at all.
    //
    // The assertion is the same on both kinds of volume, which is the point of
    // it being here: one entry afterwards, under the new spelling.
    {
        const VfsUri from = context.root.child(QStringLiteral("beta-renamed.log"));
        const VfsUri to = context.root.child(QStringLiteral("Beta-Renamed.log"));

        Result<void> renamed = fs.rename(from, to);
        QVERIFY2(renamed.ok(),
            qPrintable(QStringLiteral("a rename that only changes case must succeed: %1")
                           .arg(renamed.error().message)));

        Result<FileEntryList> listing = fs.list(context.root, CancelToken());
        QVERIFY2(listing.ok(), "listing after a case-only rename must succeed");

        int matching = 0;
        QStringList spellings;
        for (const FileEntry& entry : listing.value()) {
            if (entry.name.compare(QStringLiteral("beta-renamed.log"), Qt::CaseInsensitive) == 0) {
                ++matching;
                spellings.append(entry.name);
            }
        }
        QVERIFY2(matching == 1,
            qPrintable(QStringLiteral("one entry expected after a case-only rename, found: %1")
                           .arg(spellings.join(QStringLiteral(", ")))));
        QCOMPARE(spellings.first(), QStringLiteral("Beta-Renamed.log"));

        // Back to the name the rest of the suite goes on using.
        QVERIFY2(fs.rename(to, from).ok(), "renaming the case back must succeed too");
    }

    // --- replace ------------------------------------------------------------
    //
    // The other rename, and the difference between the two is the whole reason
    // there are two. rename() refuses an occupied destination, because a rename
    // that silently destroyed a file nobody mentioned is how the only copy of
    // something goes. replace() is the call for when the caller has already
    // established that replacing is exactly what was asked for -- a finished
    // write going over the file it was written to replace.
    //
    // What is asserted here is the outcome, not how it was reached: a backend
    // that can do it in one step and one that has to remove and then rename are
    // both correct, and only one of them is available over a protocol. See
    // ADR-0087.
    {
        const VfsUri arriving = context.root.child(QStringLiteral("replacement.bin"));
        const VfsUri standing = context.root.child(QStringLiteral("replaced.bin"));
        const QByteArray fresh = QByteArrayLiteral("the bytes that are arriving");

        const auto put = [&fs](const VfsUri& where, const QByteArray& what) {
            Result<std::unique_ptr<QIODevice>> out = fs.openWrite(where, what.size());
            if (!out.ok())
                return out.error();
            if (out.value()->write(what) != what.size())
                return VfsError::make(VfsError::IoError, QStringLiteral("short write"));
            const Result<void> closed = closeAndReport(*out.value());
            return closed.ok() ? VfsError::ok() : closed.error();
        };

        QVERIFY2(!put(standing, QByteArrayLiteral("the bytes that are there")).isError(), "seeding failed");
        QVERIFY2(!put(arriving, fresh).isError(), "seeding failed");

        const Result<void> replaced = fs.replace(arriving, standing);
        QVERIFY2(replaced.ok(),
            qPrintable(QStringLiteral("replace onto an occupied name must succeed: %1")
                           .arg(replaced.error().message)));
        QVERIFY2(!fs.stat(arriving).ok(), "the name replace() moved out of must be gone");

        Result<std::unique_ptr<QIODevice>> back = fs.openRead(standing, fresh.size());
        QVERIFY2(back.ok(), qPrintable(back.error().message));
        QCOMPARE(back.value()->readAll(), fresh);
        back.value().reset();

        // And onto a free name, which is an ordinary rename. A backend that only
        // handled the occupied case would fail here rather than where a caller
        // could see why.
        const VfsUri onward = context.root.child(QStringLiteral("replaced-again.bin"));
        QVERIFY2(fs.replace(standing, onward).ok(), "replace onto a free name is a rename");
        QVERIFY2(fs.stat(onward).ok(), "the new name must exist afterwards");

        QVERIFY(fs.remove(onward, false).ok());
    }

    // --- remove -----------------------------------------------------------
    {
        const VfsUri file = context.root.child(QStringLiteral("beta-renamed.log"));
        QVERIFY2(fs.remove(file, false).ok(), "removing a file must succeed");
        QVERIFY2(!fs.stat(file).ok(), "the file must be gone after remove");

        Result<void> missing = fs.remove(file, false);
        QVERIFY2(!missing.ok(), "removing a missing path must fail");
        QCOMPARE(missing.error().code, VfsError::NotFound);

        const VfsUri nonEmpty = context.root.child(QStringLiteral("nested"));
        Result<void> refused = fs.remove(nonEmpty, false);
        QVERIFY2(!refused.ok(), "non-recursive remove of a non-empty directory must fail");
        QCOMPARE(refused.error().code, VfsError::NotEmpty);

        QVERIFY2(fs.remove(nonEmpty, true).ok(), "recursive remove must succeed");
        QVERIFY2(!fs.stat(nonEmpty).ok(), "the directory must be gone after a recursive remove");
    }

    // ---- optional: access reporting ------------------------------------
    // A backend that advertises the capability has to answer; one that does not
    // has to say NotSupported rather than returning something empty. Either is
    // fine -- silently returning a blank answer is not.
    {
        const bool advertised = fs.capabilities().testFlag(VfsCapability::ReportsAccess);
        Result<AccessInfo> access = fs.access(context.root);

        if (advertised) {
            QVERIFY2(access.ok(), "a backend advertising ReportsAccess must answer");
            QVERIFY2(
                access.value().isKnown(), "an advertised answer has to contain something a reader can use");
        } else {
            QVERIFY2(!access.ok(), "a backend without ReportsAccess must refuse, not invent");
            QCOMPARE(access.error().code, VfsError::NotSupported);
        }
    }

    if (!context.exercisesScale)
        return;

    // ---- optional: scale -------------------------------------------------
    //
    // **The largest payload the suite ever wrote was 64 KiB**, and everything a
    // size gets wrong is above that: an offset held in 32 bits, a listing built
    // by appending to a list that is copied each time, a name one byte over what
    // the volume takes. None of the three is visible at 64 KiB, and all three
    // are what somebody hits on the day they use this on real data. Behind
    // exercisesScale because it is minutes and gigabytes, and the fast tier is
    // a tier somebody runs on every change. See MOLE-401.
    {
        // Past 4 GiB, which is where an offset kept in 32 bits stops working.
        // Not a round number: the last chunk being short is the case a loop that
        // assumes whole chunks gets wrong.
        const qint64 large = 4LL * 1024 * 1024 * 1024 + 4096 + 7;
        const VfsUri huge = context.root.child(QStringLiteral("four-gigabytes.bin"));

        const Result<SpaceInfo> room = fs.space(context.root);
        const bool enoughRoom = room.ok() && room.value().freeBytes > large + (256LL << 20);
        if (!enoughRoom) {
            qInfo("the large-file case did not run: %s",
                room.ok()
                    ? "the scratch volume has less room than the file needs"
                    : qPrintable(
                          QStringLiteral("this drive does not report space (%1)").arg(room.error().message)));
        } else {
            // A pattern rather than zeroes, so a read that lands at the wrong
            // offset comes back with the wrong bytes rather than with the same
            // zeroes the right offset would have given.
            const int chunk = 8 << 20;
            QByteArray pattern(chunk, Qt::Uninitialized);
            for (int index = 0; index < chunk; ++index)
                pattern[index] = char('A' + (index % 23));

            Result<std::unique_ptr<QIODevice>> writing = fs.openWrite(huge, large, noCancel);
            QVERIFY2(writing.ok(), qPrintable(writing.error().message));
            qint64 written = 0;
            while (written < large) {
                const qint64 want = qMin<qint64>(chunk, large - written);
                const qint64 put = writing.value()->write(pattern.constData(), want);
                QCOMPARE(put, want);
                written += put;
            }
            const Result<void> landed = closeAndReport(*writing.value());
            QVERIFY2(landed.ok(), qPrintable(landed.error().message));
            writing.value().reset();

            const Result<FileEntry> found = fs.stat(huge);
            QVERIFY2(found.ok(), qPrintable(found.error().message));
            QCOMPARE(found.value().size, large);

            // Three places: the beginning, the two bytes either side of the 4 GiB
            // line, and the short tail at the end.
            const QList<qint64> offsets { 0, (4LL * 1024 * 1024 * 1024) - 3, large - 9 };
            Result<std::unique_ptr<QIODevice>> reading = fs.openRead(huge, large, noCancel);
            QVERIFY2(reading.ok(), qPrintable(reading.error().message));
            if (fs.capabilities().testFlag(VfsCapability::RandomAccessRead)) {
                for (const qint64 offset : offsets) {
                    QVERIFY2(reading.value()->seek(offset),
                        qPrintable(QStringLiteral("this drive could not seek to %1 in a %2 byte file")
                                       .arg(offset)
                                       .arg(large)));
                    const QByteArray got = reading.value()->read(9);
                    QCOMPARE(got.size(), 9);
                    for (int index = 0; index < got.size(); ++index) {
                        const qint64 absolute = offset + index;
                        QCOMPARE(got.at(index), char('A' + ((absolute % chunk) % 23)));
                    }
                }
            } else {
                qInfo("the offsets in the large file were not checked: this drive does not seek");
            }
            reading.value().reset();

            QVERIFY2(fs.remove(huge, false, noCancel).ok(), "the large file has to be removable again");
        }
    }

    {
        // A listing wide enough that anything quadratic in the number of entries
        // shows up as a suite that never finishes rather than as a slow one.
        const int many = 100'000;
        const VfsUri wide = context.root.child(QStringLiteral("many-entries"));
        QVERIFY2(fs.makeDirectory(wide).ok(), "the wide directory has to be creatable");

        for (int index = 0; index < many; ++index) {
            const VfsUri entry
                = wide.child(QStringLiteral("entry-%1.dat").arg(index, 6, 10, QLatin1Char('0')));
            Result<std::unique_ptr<QIODevice>> opened = fs.openWrite(entry, 1, noCancel);
            QVERIFY2(opened.ok(), qPrintable(opened.error().message));
            opened.value()->write(QByteArray(1, char('x')));
            const Result<void> landed = closeAndReport(*opened.value());
            QVERIFY2(landed.ok(), qPrintable(landed.error().message));
            opened.value().reset();
        }

        const Result<FileEntryList> listing = fs.list(wide, noCancel);
        QVERIFY2(listing.ok(), qPrintable(listing.error().message));
        QCOMPARE(listing.value().size(), many);

        // Not only the count: a listing that dropped one and invented another
        // has the right size and the wrong contents.
        QSet<QString> names;
        for (const FileEntry& entry : listing.value())
            names.insert(entry.name);
        QCOMPARE(names.size(), many);
        QVERIFY(names.contains(QStringLiteral("entry-000000.dat")));
        QVERIFY(names.contains(QStringLiteral("entry-099999.dat")));

        QVERIFY2(fs.remove(wide, true, noCancel).ok(), "the wide directory has to be removable again");
    }

    {
        // A name at the limit and a name one past it. The limit is the drive's
        // own answer, so this is the same assertion on every volume rather than
        // 255 typed into a test.
        const NameRules rules = fs.nameRules();
        const int limit = rules.maximumLengthInBytes > 0 ? rules.maximumLengthInBytes : rules.maximumLength;
        if (limit <= 0) {
            qInfo("the name-length case did not run: this drive states no limit");
        } else {
            // ASCII, so bytes and characters are the same number whichever of
            // the two limits the drive stated.
            const QString atTheLimit = QString(limit - 4, QLatin1Char('n')) + QStringLiteral(".dat");
            const VfsUri accepted = context.root.child(atTheLimit);
            Result<std::unique_ptr<QIODevice>> opened = fs.openWrite(accepted, 1, noCancel);
            QVERIFY2(opened.ok(),
                qPrintable(QStringLiteral("a name of exactly %1 was refused, and this drive says %1 is "
                                          "the limit: %2")
                               .arg(limit)
                               .arg(opened.ok() ? QString() : opened.error().message)));
            opened.value()->write(QByteArray(1, char('x')));
            const Result<void> landed = closeAndReport(*opened.value());
            QVERIFY2(landed.ok(),
                qPrintable(QStringLiteral("a name of exactly %1 could not be committed, and this drive "
                                          "says %1 is the limit: %2")
                               .arg(limit)
                               .arg(landed.error().message)));
            opened.value().reset();
            QVERIFY2(fs.stat(accepted).ok(), "a name at the limit has to be found again under that name");
            QVERIFY2(fs.remove(accepted, false, noCancel).ok(), "and has to be removable");

            const QString pastIt = QString(limit + 1, QLatin1Char('n'));
            Result<std::unique_ptr<QIODevice>> refused
                = fs.openWrite(context.root.child(pastIt), 1, noCancel);
            if (refused.ok()) {
                // **A staging backend refuses on the way out, not on the way
                // in**: the working name is short enough to open and the rename
                // into place is what fails. So the refusal is collected from
                // closeAndReport() -- and either it says no, or the file is
                // there under the name that was asked for. What is ruled out is
                // the third answer: an acknowledged write that kept the bytes
                // under a name nobody asked for, which is ADR-0070's silent
                // rewrite.
                const Result<void> landed = closeAndReport(*refused.value());
                refused.value().reset();
                if (landed.ok()) {
                    QVERIFY2(fs.stat(context.root.child(pastIt)).ok(),
                        "this drive acknowledged a name past its own limit and did not keep it under "
                        "that name");
                    fs.remove(context.root.child(pastIt), false, noCancel);
                } else {
                    QVERIFY2(!fs.stat(context.root.child(pastIt)).ok(),
                        "this drive refused the name and left the file behind anyway");
                }
            } else {
                QVERIFY2(refused.error().code != VfsError::Unknown,
                    "a name refused for its length has to say why");
            }
        }
    }
}

} // namespace mole::test
