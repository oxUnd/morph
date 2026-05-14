set(MORPH_SOURCES
	main.c
)

set(MORPH_LIBS
	morph-util
	morph-db
	morph-http
	morph-agent
	morph-models
	morph-ext
	morph-ipc
	morph-tools
	morph-config
	morph-session
	morph-skill
	SQLite::SQLite3
	CURL::libcurl
)

add_library(morph-toml STATIC
	${CMAKE_SOURCE_DIR}/vendor/toml.c
)
target_include_directories(morph-toml PUBLIC
	${CMAKE_SOURCE_DIR}/vendor
)
target_compile_options(morph-toml PRIVATE
	-Wno-implicit-function-declaration
	-Wno-implicit-int
	-Wno-sign-conversion
	-Wno-shorten-64-to-32
	-Wno-empty-body
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/vendor/toml.c PROPERTIES
	COMPILE_FLAGS "-include ${CMAKE_CURRENT_SOURCE_DIR}/vendor_toml_compat.h"
)

add_library(morph-config STATIC
	config.c
)
target_include_directories(morph-config PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-config PUBLIC morph-util morph-toml)

add_library(morph-session STATIC
	session.c
)
target_include_directories(morph-session PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-session PUBLIC morph-util morph-db)

add_subdirectory(render)
add_subdirectory(agent)
add_subdirectory(sandbox)
add_subdirectory(ext)
add_subdirectory(skill)
add_subdirectory(ipc)
add_subdirectory(models)
add_subdirectory(http)
add_subdirectory(db)
add_subdirectory(util)

add_library(morph-cli STATIC
	cli.c
)
target_include_directories(morph-cli PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-cli PUBLIC morph-util morph-db morph-agent morph-http morph-models morph-config morph-session morph-render morph-skill)

add_executable(morph ${MORPH_SOURCES})
set_target_properties(morph PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_include_directories(morph PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph ${MORPH_LIBS} morph-cli)

find_path(READLINE_INCLUDE_DIR readline/readline.h
	/opt/homebrew/opt/readline/include
	/usr/local/opt/readline/include
	/usr/include
)
find_library(READLINE_LIBRARY readline
	/opt/homebrew/opt/readline/lib
	/usr/local/opt/readline/lib
	/usr/lib
)
if(READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
	target_include_directories(morph PRIVATE ${READLINE_INCLUDE_DIR})
	target_link_libraries(morph ${READLINE_LIBRARY})
	add_compile_definitions(HAVE_READLINE=1)
	message(STATUS "readline found: ${READLINE_LIBRARY}")
else()
	message(STATUS "readline not found, using fgets")
endif()