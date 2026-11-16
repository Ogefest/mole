#pragma once

#include "core/data/ITableSource.h"

#include <QString>
#include <QStringList>

/// The whole of what crosses between Mole and its Parquet module.
///
/// **Parquet is a module loaded at run time, not a library the application
/// links.** A binary linked to Arrow cannot start where Arrow is missing, and the
/// one .deb is for three distributions of which only Ubuntu 26.04 packages Arrow.
/// So the Arrow half lives in `mole-parquet.so`, built where Arrow is, and the
/// application asks for it by name: present and loadable, the preview has a grid;
/// absent or unloadable, ParquetTable says it is unsupported and the preview falls
/// back to the file-information view. See ADR-0106 and MOLE-459.
///
/// **Keep this boundary narrow.** The module and the application can be built by
/// different compilers against different Qt minor versions -- the .deb's module is
/// built on Ubuntu 26.04 against Qt 6.10 and the application on 24.04 against 6.4 --
/// so nothing crosses it but this interface, Qt's own value types, and the four C
/// functions below. Qt keeps binary compatibility within a major version; nothing
/// of Mole's own crosses except this header, which is all virtual calls.
///
/// Changing anything here -- a method, its order, a parameter -- is changing the
/// ABI the module is built against: raise MOLE_PARQUET_MODULE_ABI with it, and a
/// module built against the old one is refused rather than called wrongly.
namespace mole {

/// One Parquet file, read in place. Implemented by the module; ParquetTable is the
/// application's side and forwards to it.
class ParquetReader : public ITableSource
{
public:
    ~ParquetReader() override = default;

    /// Opens the file and reads its footer. False with a reason when the file
    /// cannot be read as Parquet.
    virtual bool open(QString* errorOut) = 0;

    /// What the file says about itself: row groups, compression, the writer.
    virtual QString fileSummary() const = 0;

    /// Column types as Arrow names them.
    virtual QStringList columnTypes() const = 0;

    /// Rows decoded since the file was opened -- see ParquetTable::rowsDecoded().
    virtual qint64 rowsDecoded() const = 0;
};

} // namespace mole

/// Raised whenever ParquetReader or ITableSource changes shape.
#define MOLE_PARQUET_MODULE_ABI 1

// What the module exports, by these names and with C linkage so the names are the
// same whatever compiled it.
//
//   int mole_parquet_module_abi()               MOLE_PARQUET_MODULE_ABI it was built with
//   int mole_parquet_module_qt_version()        QT_VERSION it was built against
//   const char* mole_parquet_module_arrow()     the Arrow version it links, as Arrow spells it
//   mole::ParquetReader* mole_parquet_module_create(const QString* path)
//
// The reader is destroyed through its virtual destructor, which lives in the
// module, so memory the module allocated is freed by the module.
extern "C" {
using MoleParquetModuleAbiFn = int (*)();
using MoleParquetModuleQtVersionFn = int (*)();
using MoleParquetModuleArrowFn = const char* (*)();
using MoleParquetModuleCreateFn = mole::ParquetReader* (*)(const QString* path);
}
