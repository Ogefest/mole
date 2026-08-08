# Host side of the extension system: the registries plugins write into and the
# loader that finds them.
add_library(mole_host STATIC
    FeatureRegistry.h
    FeatureRegistry.cpp
    PreviewRegistry.h
    PreviewRegistry.cpp
    ActionRegistry.h
    ActionRegistry.cpp
    PluginManager.h
    PluginManager.cpp
)

add_library(mole::host ALIAS mole_host)

target_include_directories(mole_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mole_host
    PUBLIC mole_sdk
    PRIVATE mole_flags)
