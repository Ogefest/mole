#include "FileSystemConformance.h"

#include "core/vfs/PartialWrite.h"

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
    {
        CancelToken cancelled;
        cancelled.cancel();
        Result<FileEntryList> listing = fs.list(context.root, cancelled);
        QVERIFY2(!listing.ok(), "a pre-cancelled token must abort the listing");
        QCOMPARE(listing.error().code, VfsError::Cancelled);
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
}

} // namespace mole::test
