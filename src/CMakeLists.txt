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

find_package(Readline)
if(Readline_FOUND)
	target_link_libraries(multi-agent Readline::Readline)
endif()