# morph-fastcgi — FastCGI front-end for the Morph agent
#
# Builds a single executable `morph-fastcgi` that links against the static
# libraries already produced by the parent project (morph-util, morph-db,
# morph-session, morph-agent, etc).  Optional; gated on BUILD_FASTCGI.

find_package(PkgConfig QUIET)
set(_FCGI_FOUND FALSE)

if(PkgConfig_FOUND)
    pkg_check_modules(FCGI QUIET fcgi)
    if(FCGI_FOUND)
        set(_FCGI_FOUND TRUE)
    endif()
endif()

# Fallback: probe the legacy header location directly.
if(NOT _FCGI_FOUND)
    find_path(FCGI_INCLUDE_DIR fcgiapp.h)
    find_library(FCGI_LIBRARY NAMES fcgi)
    if(FCGI_INCLUDE_DIR AND FCGI_LIBRARY)
        set(FCGI_INCLUDE_DIRS "${FCGI_INCLUDE_DIR}")
        set(FCGI_LIBRARIES    "${FCGI_LIBRARY}")
        set(_FCGI_FOUND TRUE)
    endif()
endif()

if(NOT _FCGI_FOUND)
    message(WARNING
        "libfcgi not found — skipping morph-fastcgi.  "
        "Install fcgi2 (e.g. apt install libfcgi-dev) and re-configure.")
    return()
endif()

set(FCGI_SOURCES
    fcgi_io.c
    security.c
    session_store.c
    auth.c
    router.c
    event_sink.c
    action_pump.c
    agent_bridge.c
    main.c
    handlers/health.c
    handlers/users.c
    handlers/sessions.c
    handlers/turns.c
    handlers/canvas.c
    handlers/actions.c
    handlers/artifacts.c
    handlers/events.c)

add_executable(morph-fastcgi ${FCGI_SOURCES})
set_target_properties(morph-fastcgi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
add_dependencies(morph-fastcgi morph-data)

target_include_directories(morph-fastcgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${FCGI_INCLUDE_DIRS})

# Reuse the existing morph static libraries.  Names match the parent CMake
# tree; if a target hasn't been registered (e.g. a pared-down build) the
# linker line silently drops it via the conditional below.
set(_MORPH_LIBS
    morph-agent
    morph-skill
    morph-mcp
    morph-tools
    morph-ext
    morph-ipc
    morph-models
    morph-session
    morph-credits
    morph-http
    morph-toml
    morph-config
    morph-db
    morph-event
    morph-util)

foreach(_lib ${_MORPH_LIBS})
    if(TARGET ${_lib})
        target_link_libraries(morph-fastcgi PRIVATE ${_lib})
    endif()
endforeach()

target_link_libraries(morph-fastcgi PRIVATE
    ${FCGI_LIBRARIES}
    morph-runtime
    pthread
    sqlite3
    m)

if(FCGI_LIBRARY_DIRS)
    target_link_directories(morph-fastcgi PRIVATE ${FCGI_LIBRARY_DIRS})
endif()
if(FCGI_CFLAGS_OTHER)
    target_compile_options(morph-fastcgi PRIVATE ${FCGI_CFLAGS_OTHER})
endif()

target_compile_options(morph-fastcgi PRIVATE -Wall -Wextra -Wno-unused-parameter)

install(TARGETS morph-fastcgi
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
