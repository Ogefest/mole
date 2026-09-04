// A plugin built against an older API, for the check that has to refuse it.
//
// **Its `metadata()` aborts.** That is the assertion: the host used to decide
// whether a plugin could be spoken to by speaking to it, and this fixture makes
// that visible -- if anything asks this library what it is, the test process dies
// rather than passing. What must happen instead is that the interface identifier
// below is read out of the library's Qt metadata and the library is put down
// again, with nothing in it having run.
//
// The identifier is written out rather than built from MOLE_PLUGIN_API_VERSION,
// because the whole point is that it does not match: a fixture that followed the
// host's version would stop testing anything the day the version changed.
//
// See MOLE-366 and ADR-0098.

#include "sdk/PluginApi.h"

#include <QObject>
#include <QtGlobal>

namespace {

class StaleApiPlugin : public QObject, public mole::IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "io.github.ogefest.mole.Plugin/1")
    Q_INTERFACES(mole::IPlugin)

public:
    mole::PluginMetadata metadata() const override
    {
        qFatal("a plugin built against another API version was asked what it is; "
               "the version has to be read from its metadata instead");
        return {};
    }

    void registerExtensions(mole::PluginRegistry&) override
    {
        qFatal("a plugin built against another API version was allowed to register");
    }
};

} // namespace

#include "StaleApiPlugin.moc"
