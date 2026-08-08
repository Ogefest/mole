#include "FileSystemConformance.h"

#include <QTest>

#include <algorithm>

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

    // --- reading content --------------------------------------------------
    {
        Result<std::unique_ptr<QIODevice>> device
            = fs.openRead(context.root.child(QStringLiteral("alpha.txt")));
        QVERIFY2(device.ok(), qPrintable(device.error().message));
        QCOMPARE(device.value()->readAll(), QByteArray("hello"));
    }

    if (!context.expectsWriteSupport)
        return;

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
