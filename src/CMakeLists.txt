set(MULTI_AGENT_SOURCES
	main.c
)

set(MULTI_AGENT_LIBS
	multi-agent-util
	multi-agent-db
	multi-agent-http
	multi-agent-agent
	multi-agent-models
	multi-agent-skill
	multi-agent-ipc
	multi-agent-tools
	multi-agent-config
	multi-agent-session
	SQLite::SQLite3
	CURL::libcurl
)

add_library(multi-agent-config STATIC
	config.c
)
target_include_directories(multi-agent-config PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-config PUBLIC multi-agent-util)

add_library(multi-agent-session STATIC
	session.c
)
target_include_directories(multi-agent-session PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-session PUBLIC multi-agent-util multi-agent-db)

add_subdirectory(render)
add_subdirectory(agent)
add_subdirectory(sandbox)
add_subdirectory(skill)
add_subdirectory(ipc)
add_subdirectory(models)
add_subdirectory(http)
add_subdirectory(db)
add_subdirectory(util)

add_library(multi-agent-cli STATIC
	cli.c
)
target_include_directories(multi-agent-cli PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-cli PUBLIC multi-agent-util multi-agent-db multi-agent-agent multi-agent-http multi-agent-models multi-agent-config multi-agent-session multi-agent-render)

add_executable(multi-agent ${MULTI_AGENT_SOURCES})
target_include_directories(multi-agent PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent ${MULTI_AGENT_LIBS} multi-agent-cli)

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
	target_include_directories(multi-agent PRIVATE ${READLINE_INCLUDE_DIR})
	target_link_libraries(multi-agent ${READLINE_LIBRARY})
	add_compile_definitions(HAVE_READLINE=1)
	message(STATUS "readline found: ${READLINE_LIBRARY}")
else()
	message(STATUS "readline not found, using fgets")
endif()