# Presentation logic: list models and controllers. No QML files and no QML type
# registration, so every class here is constructible from a plain test binary.
add_library(mole_ui STATIC
    AppController.h
    AppController.cpp
    Palette.h
    Palette.cpp
    DragSource.h
    DragSource.cpp
    FileLauncher.h
    FileLauncher.cpp
    SessionStore.h
    SessionStore.cpp
    UpdateCheck.h
    UpdateCheck.cpp

    models/CommandPaletteModel.h
    models/CommandPaletteModel.cpp
    models/BookmarkModel.h
    models/BookmarkModel.cpp
    models/FileListModel.h
    models/FileListModel.cpp
    models/TableModel.h
    models/TableModel.cpp
    models/BreakdownModel.h
    models/BreakdownModel.cpp
    models/DuplicateGroupModel.h
    models/DuplicateGroupModel.cpp
    models/BrowserPaneController.h
    models/BrowserPaneController.cpp
    ThumbnailCache.h
    ThumbnailCache.cpp
    ThumbnailSource.h
    ThumbnailSource.cpp

    models/RepositoryInfo.h
    models/RepositoryInfo.cpp
    models/DriveListModel.h
    models/DriveListModel.cpp
    models/TerminalController.h
    models/TerminalController.cpp
    models/TaskListModel.h
    models/TaskListModel.cpp
    models/TabsModel.h
    models/TabsModel.cpp
)

add_library(mole::ui ALIAS mole_ui)

target_include_directories(mole_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# QtGui only for QDesktopServices; there is still no widget or window here, so
# the tests keep running headless.
#
# Network is the update check and nothing else -- one conditional GET of the
# release manifest, on the event loop, at startup. PRIVATE because no header here
# mentions it: UpdateCheck.h forward-declares what it holds, so nothing that links
# this library has to know Qt Network exists, while the library itself still
# carries the link. It stays out of mole_core deliberately, so the console runner
# and every headless tool do not acquire an HTTP stack for a window's benefit.
target_link_libraries(mole_ui
    PUBLIC mole_host Qt6::Gui
    PRIVATE mole_flags Qt6::Network)
