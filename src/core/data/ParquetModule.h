#pragma once

#include "core/data/ParquetReader.h"

#include <QString>
#include <QStringList>

#include <memory>

namespace mole {

/// Finding and loading `mole-parquet.so`, the Arrow half of the Parquet preview.
///
/// Loaded once per process, the first time anything asks, and never unloaded: a
/// reader it made may be alive until the application exits. A module that cannot
/// be loaded says why once, in the session log, and from then on Parquet is simply
/// unsupported -- the preview falls back to the file-information view, which is the
/// rule for every optional library. See ParquetReader.h and MOLE-459.
namespace parquetModule {

    /// What happened when the module was looked for.
    struct State
    {
        /// True when the module is loaded and safe to call.
        bool loaded = false;
        /// The file that was loaded, or the one that was tried and refused.
        QString path;
        /// The Arrow version the module links, when it loaded.
        QString arrowVersion;
        /// Why it is not loaded, in words for `mole --plugins` and the log. Empty
        /// when it is.
        QString reason;
        /// The factory, when it is loaded.
        MoleParquetModuleCreateFn create = nullptr;
    };

    /// The module this process uses, loaded on first call. Thread-safe.
    const State& current();

    /// Where the module is looked for, in order: `MOLE_PARQUET_MODULE` when it is
    /// set -- a file, and then nothing else is tried, which is how a test makes
    /// the module absent -- and otherwise `modules/` beside the executable and one
    /// level up (a build tree, and its tests), then `<prefix>/lib/mole/modules` and
    /// `lib64` beside the installed binary.
    QStringList candidates();

    /// Loads the first of `candidates` that exists. What current() does, given the
    /// list rather than working it out -- here so a test can hand it a file.
    State loadFrom(const QStringList& candidates);

    /// Turns what the dynamic loader said into a reason somebody can act on. Three
    /// failures look alike from outside and mean different things: Arrow is not
    /// installed, the module was built for a newer system than this one, or the file
    /// is not a module at all.
    QString explainLoadFailure(const QString& loaderMessage);

    /// One line for `mole --plugins`: `parquet: module loaded, Arrow 23.0.1 (...)`
    /// or `parquet: not available -- <reason>`.
    QString diagnosticLine();

    /// A reader for `path` from the loaded module, or null when there is none.
    std::unique_ptr<ParquetReader> createReader(const QString& path);

} // namespace parquetModule

} // namespace mole
