// A Parquet module that loads and must still be refused.
//
// Built twice by tests/CMakeLists.txt, each time lying about one thing: the Qt it
// was built against (newer than any that exists), or the module interface it
// speaks. It links Qt Core for its headers and nothing else, and it never makes a
// reader. See tst_ParquetModule and MOLE-459.

#include "core/data/ParquetReader.h"

#include <QtGlobal>

extern "C" {

Q_DECL_EXPORT int mole_parquet_module_abi()
{
    return FAKE_MODULE_ABI;
}

Q_DECL_EXPORT int mole_parquet_module_qt_version()
{
    return FAKE_MODULE_QT_VERSION;
}

Q_DECL_EXPORT const char* mole_parquet_module_arrow()
{
    return "0.0.0-fake";
}

Q_DECL_EXPORT mole::ParquetReader* mole_parquet_module_create(const QString*)
{
    return nullptr;
}

} // extern "C"
