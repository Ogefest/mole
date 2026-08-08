#pragma once

#include "core/CoreMetaTypes.h"

#include <QCoreApplication>
#include <QTest>

/// Entry point for every test binary.
///
/// QTEST_GUILESS_MAIN would do, but the core ships value types that travel
/// through queued signals; forgetting to register them turns into a confusing
/// runtime warning instead of a compile error. Doing it here means no test can
/// get it wrong.
#define MOLE_TEST_MAIN(TestClass)                                                                             \
    int main(int argc, char** argv)                                                                          \
    {                                                                                                        \
        QCoreApplication app(argc, argv);                                                                    \
        app.setOrganizationName("Mole");                                                         \
        app.setApplicationName("mole-tests");                                                                 \
        mole::registerCoreMetaTypes();                                                                        \
        TestClass testObject;                                                                                \
        QTEST_SET_MAIN_SOURCE_PATH                                                                           \
        return QTest::qExec(&testObject, argc, argv);                                                        \
    }
