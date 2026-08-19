# The published plugin API. Everything a third-party plugin is allowed to see
# lives here, and it deliberately depends on nothing but core, QtCore and QtGui --
# a plugin must not have to link the shell in order to extend it.
#
# QtGui arrived with IThumbnailer, because a thumbnail is a QImage and QImage is
# QtGui. It is the image, font and colour layer and brings no widget and no
# window, which is why the whole suite still runs headless. See
# docs/adr/0058-a-file-can-say-what-it-looks-like.md.
add_library(mole_sdk STATIC
    PluginApi.h
    PluginServices.h
    IFeature.h
    IPreviewProvider.h
    IPreviewProvider.cpp
    IMetadataReader.h
    IThumbnailer.h
    FeatureController.h
    FeatureController.cpp
    ScanReaders.h
    ScanReaders.cpp
)

add_library(mole::sdk ALIAS mole_sdk)

target_include_directories(mole_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mole_sdk
    PUBLIC mole_core Qt6::Core Qt6::Gui
    PRIVATE mole_flags)
