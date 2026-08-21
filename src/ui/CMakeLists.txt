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
target_link_libraries(mole_ui
    PUBLIC mole_host Qt6::Gui
    PRIVATE mole_flags)
