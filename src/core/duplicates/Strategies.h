#pragma once

#include "core/duplicates/DuplicateStrategy.h"

namespace mole {

/// Same size. One pass over metadata, no reading at all.
///
/// Useless on its own for proving files identical, and genuinely useful for
/// finding candidates on a slow drive before deciding what to read.
class SameSizeStrategy final : public IDuplicateStrategy
{
public:
    QString id() const override { return QStringLiteral("size"); }
    QString label() const override { return QStringLiteral("Same size"); }
    QString description() const override
    {
        return QStringLiteral("Groups files with identical byte counts. Instant, and a "
                              "starting point rather than an answer -- different files often "
                              "share a size.");
    }
    QStringList stageNames() const override { return { QStringLiteral("size") }; }
    bool stageReadsContent(int) const override { return false; }
    QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const override;
};

/// Same name, whatever the contents.
///
/// The strategy for "I have copied this folder around and want to know where
/// else each file turned up", which no content comparison answers.
class SameNameStrategy final : public IDuplicateStrategy
{
public:
    QString id() const override { return QStringLiteral("name"); }
    QString label() const override { return QStringLiteral("Same name"); }
    QString description() const override
    {
        return QStringLiteral("Groups files sharing a filename, ignoring case and contents. "
                              "Finds copies that were edited apart, which a content scan "
                              "cannot.");
    }
    QStringList stageNames() const override { return { QStringLiteral("name") }; }
    bool stageReadsContent(int) const override { return false; }
    QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const override;
};

/// Same name and same size. Cheap, and much less noisy than either alone.
class SameNameAndSizeStrategy final : public IDuplicateStrategy
{
public:
    QString id() const override { return QStringLiteral("name+size"); }
    QString label() const override { return QStringLiteral("Same name and size"); }
    QString description() const override
    {
        return QStringLiteral("Both together. Still no proof the contents match, but wrong "
                              "far less often than either on its own, and just as fast.");
    }
    QStringList stageNames() const override { return { QStringLiteral("size"), QStringLiteral("name") }; }
    bool stageReadsContent(int) const override { return false; }
    QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const override;
};

/// Identical contents, proven.
///
/// Three stages: size, then a hash of the first megabyte, then a hash of the
/// whole file. Most non-duplicates are ruled out by the first two, so the
/// expensive pass runs on very little -- which is the difference between a scan
/// that finishes and one that does not.
class SameContentStrategy final : public IDuplicateStrategy
{
public:
    QString id() const override { return QStringLiteral("content"); }
    QString label() const override { return QStringLiteral("Identical contents"); }
    QString description() const override
    {
        return QStringLiteral("Proves it: same size, same first 1 MB, same hash of the whole "
                              "file. Reads only what survives each step, so it costs far less "
                              "than hashing everything.");
    }
    QStringList stageNames() const override
    {
        return { QStringLiteral("size"), QStringLiteral("first 1 MB"), QStringLiteral("whole file") };
    }
    bool stageReadsContent(int stage) const override { return stage > 0; }
    QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const override;

    /// How much the second stage reads.
    ///
    /// A megabyte, raised from 16 kB on 2026-08-18 because 16 kB was not enough to
    /// separate the files people actually have a lot of. A video container, a RAW
    /// photograph, a PDF and a virtual disk image all carry headers and index
    /// tables far larger than that, so genuinely different files of the same size
    /// agreed at this stage and every one of them went through to the whole-file
    /// hash -- which is the pass this stage exists to keep small.
    ///
    /// It is not free, and the honest accounting is this: the stage reads 64 times
    /// what it did, but only ever for files that already share an exact size, and
    /// a file no larger than this skips the stage entirely rather than being read
    /// twice. What it buys is that the whole-file pass -- which reads gigabytes,
    /// not megabytes -- sees far less.
    static constexpr qint64 kHeadBytes = 1024 * 1024;
};

} // namespace mole
