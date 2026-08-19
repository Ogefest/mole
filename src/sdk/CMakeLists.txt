# The published plugin API. Everything a third-party plugin is allowed to see
# lives here, and it deliberately depends on nothing but core and QtCore -- a
# plugin must not have to link the shell in order to extend it.
add_library(mole_sdk STATIC
    PluginApi.h
    PluginServices.h
    IFeature.h
    IPreviewProvider.h
    IPreviewProvider.cpp
    IMetadataReader.h
    FeatureController.h
    FeatureController.cpp
    ScanReaders.h
    ScanReaders.cpp
)

add_library(mole::sdk ALIAS mole_sdk)

target_include_directories(mole_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mole_sdk
    PUBLIC mole_core Qt6::Core
    PRIVATE mole_flags)
