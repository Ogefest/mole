#pragma once

#include "sdk/IArchiver.h"
#include "sdk/PluginServices.h"

namespace mole {

/// What this plugin can pack, and the packing itself, offered through the SDK.
///
/// **The shell used to call CompressTask's statics directly**, from seven
/// members of AppController behind `#ifdef MOLE_HAVE_ARCHIVE`. Everything they
/// asked is answered here instead: the format table is this plugin's own, and
/// the work is submitted on this plugin's own task manager. See ADR-0101 and
/// MOLE-415.
class CompressArchiver final : public IArchiver
{
public:
    explicit CompressArchiver(PluginServices services);

    QList<Format> formats() const override;
    bool compress(const Request& request) override;

private:
    PluginServices m_services;
};

} // namespace mole
