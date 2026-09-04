// A plugin compiled against a PluginServices two fields shorter than the host's.
//
// PluginServices says of itself that fields may be appended without bumping the
// API version, and it has been appended to twice since the version last moved.
// This fixture is what makes that promise assertable: it is compiled with
// tests/support/plugins/older-sdk/ ahead of src/, so its translation unit sees
// the struct as it was two appends ago -- twelve pointers where the host has
// fourteen.
//
// What it does is read every field it knows about and report what it saw, so the
// test can assert that a shorter view of the host's own object reads the right
// pointers. Passed by value that held only because a trivially copyable struct of
// this size goes in memory on every current ABI; passed by reference, which is
// what IMetadataReader and IThumbnailer now take, it holds because the prefix of
// a struct is the same struct.
//
// It registers nothing. Reporting through reportError() keeps the fixture down to
// the interface headers and Qt -- a fixture that registered a real reader would
// have to link the host's core, and then the .so would hold two definitions of
// PluginServices for reasons that have nothing to do with the question.
//
// See MOLE-366 and ADR-0098.

#include "sdk/PluginApi.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace {

class ShortServicesPlugin : public QObject, public mole::IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MOLE_PLUGIN_IID)
    Q_INTERFACES(mole::IPlugin)

public:
    mole::PluginMetadata metadata() const override
    {
        mole::PluginMetadata about;
        about.id = QStringLiteral("test.short-services");
        about.name = QStringLiteral("Built against a shorter PluginServices");
        about.version = QStringLiteral("1");
        about.author = QStringLiteral("the test suite");
        about.description = QStringLiteral("Reports what it can see of the host's services.");
        return about;
    }

    void registerExtensions(mole::PluginRegistry& registry) override
    {
        // Read through the shorter view. Every one of these is a field this
        // translation unit knows about; the host has two more after them.
        const mole::PluginServices& services = registry.services();

        QStringList seen;
        const struct
        {
            const char* name;
            const void* pointer;
        } fields[] = {
            { "vfs", services.vfs },
            { "tasks", services.tasks },
            { "index", services.index },
            { "events", services.events },
            { "previews", services.previews },
            { "metadata", services.metadata },
            { "thumbnails", services.thumbnails },
            { "scheduler", services.scheduler },
            { "alerts", services.alerts },
            { "reports", services.reports },
            { "sets", services.sets },
            { "preferences", services.preferences },
        };
        // The addresses and not merely which ones were set: a reordering of the
        // struct would leave the same fields non-null and put them in different
        // places, and only the values say so.
        for (const auto& field : fields) {
            seen.append(QStringLiteral("%1=%2").arg(QString::fromLatin1(field.name),
                QString::number(reinterpret_cast<quintptr>(field.pointer), 16)));
        }

        // Not an error, and reportError is the one way a plugin can say something
        // the host keeps. The test reads it back and compares every address with
        // what it handed the host.
        registry.reportError(QStringLiteral("services seen: %1").arg(seen.join(QLatin1Char(' '))));
    }
};

} // namespace

#include "ShortServicesPlugin.moc"
