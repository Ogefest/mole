#pragma once

namespace mole {

/// Registers the core value types with Qt's meta-object system so they can
/// cross thread boundaries in queued signals. Call once at startup -- the
/// application does it in main(), the test harness in its own entry point.
/// Calling it more than once is harmless.
void registerCoreMetaTypes();

} // namespace mole
