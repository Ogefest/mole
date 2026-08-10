#pragma once

#include <QString>

namespace mole::test {

/// Files for the viewers that read tabular data, in one place.
///
/// Two suites want them: the one that tests the readers, and the one that
/// photographs the previews for the user guide. A second copy of a SQLite
/// builder is a second thing to keep in step with what the viewer expects, and
/// the guide's picture is only worth having if it is of the same fixture the
/// reader is tested against.
namespace fixtures {

    /// A small database with two tables and a view: a `people` table with a null
    /// and an empty string in it, a second table whose name is a reserved word,
    /// and a view over the first. Returns false if Qt's SQLite driver is absent.
    bool writeSqlite(const QString& path);

    /// A Parquet file of `rows` rows in small row groups, so a windowed read has
    /// several to choose between. Returns false when the build has no Arrow --
    /// callers skip rather than fail, the way `ParquetTable` itself does.
    bool writeParquet(const QString& path, int rows = 5000);
    /// Whether this build can write one at all.
    bool parquetAvailable();

    /// A PNG larger than any preview pane, drawn rather than committed: no
    /// licence question, no weight in the repository, and it can be made to show
    /// what the viewer does with a picture bigger than the space it has.
    bool writeLargeImage(const QString& path);

} // namespace fixtures

} // namespace mole::test
