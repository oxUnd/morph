add_library(morph-config STATIC
	config/config.c
	config/edit.c
	config/schema.c
)
target_include_directories(morph-config PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/config
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-config PUBLIC nbase::nbase morph-cjson morph-permission)

add_library(morph-session STATIC
	session.c
	session_history.c
)
target_include_directories(morph-session PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-session PUBLIC nbase::nbase morph-cjson morph-db)

add_library(morph-credits STATIC
	credits.c
)
target_include_directories(morph-credits PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-credits PUBLIC nbase::nbase morph-cjson morph-db morph-config morph-persistence)

add_subdirectory(media)
add_subdirectory(render)
add_subdirectory(prompt)
add_subdirectory(agent)
add_subdirectory(permission)
add_subdirectory(sandbox)
add_subdirectory(exec)
add_subdirectory(ext)
add_subdirectory(skill)
add_subdirectory(mcp)
add_subdirectory(ipc)
add_subdirectory(js_runner)
add_subdirectory(models)
add_subdirectory(http)
add_subdirectory(db)
add_subdirectory(persistence)
add_subdirectory(event)
add_subdirectory(agent_ui)
add_subdirectory(sync)
add_subdirectory(runtime)
add_subdirectory(tui)
add_subdirectory(sapi)
