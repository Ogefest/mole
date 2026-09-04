# The console runner. Everything the application can do to files, with no
# window anywhere near it -- see docs/adr/0028-a-console-runner-for-the-tasks.md.
#
# The work is a library and the executable is four lines of it, so the commands
# can be driven by the test suite in-process: a runner tested only by starting
# processes is a runner whose failures are hard to read.
add_library(mole_tasks_cli STATIC
    tasks/ToolEnvironment.h
    tasks/ToolEnvironment.cpp
    tasks/Commands.h
    tasks/Commands.cpp
)

target_include_directories(mole_tasks_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# mole_host for the plugin loader, and nothing above it. No mole_ui, no
# mole_builtin, no Qt Quick: linking the interface would make "runs with no
# display" a matter of luck rather than of construction.
target_link_libraries(mole_tasks_cli
    PUBLIC mole_core mole_host
    PRIVATE mole_flags)

# Archives are packed by a task rather than by a drive, so the writer is linked
# here for the same reason mole_ui links it. The command says so when the build
# does not have it.
if(MOLE_HAVE_ARCHIVE_PLUGIN)
    target_link_libraries(mole_tasks_cli PUBLIC mole_archive_backend)
    target_compile_definitions(mole_tasks_cli PUBLIC MOLE_HAVE_ARCHIVE)
endif()

qt_add_executable(mole-tasks tasks/main.cpp)
target_link_libraries(mole-tasks PRIVATE mole_tasks_cli mole_flags)

set_target_properties(mole-tasks PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# **Exported symbols, and here it is not about backtraces.** `mole` has this for
# the names in a crash report (see src/app/CMakeLists.txt); this host needs it for
# a different reason, and until MOLE-392 it did not have it.
#
# A plugin is a shared library that statically links mole_core, so it carries its
# own copy of core -- its own function-local statics, its own logging category
# objects. When the host exports its symbols the dynamic linker resolves the
# plugin's core references to the host's copy, and there is one core in the
# process. When it does not, there are two: a plugin loaded by `mole` and the same
# binary loaded by `mole-tasks` were in different linking regimes, which is the
# kind of difference that produces a fault in one host and not the other and
# nothing to explain it.
#
# Two hosts, one regime. The real answer is a shared core -- ADR-0099 says why
# that is a separate decision and what it would take.
set_target_properties(mole-tasks PROPERTIES ENABLE_EXPORTS ON)

# The drives it can reach are the plugins it can find, which are the ones the
# window finds -- alongside the executable.
if(MOLE_HAVE_ARCHIVE_PLUGIN)
    add_dependencies(mole-tasks mole_plugin_archive)
endif()
if(MOLE_HAVE_NETWORK_PLUGIN)
    add_dependencies(mole-tasks mole_plugin_network)
endif()
