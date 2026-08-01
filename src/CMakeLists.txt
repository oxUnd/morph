add_library(morph-toml STATIC
	${CMAKE_SOURCE_DIR}/vendor/tomlc17.c
)
target_include_directories(morph-toml PUBLIC
	${CMAKE_SOURCE_DIR}/vendor
)
target_compile_options(morph-toml PRIVATE
	-Wno-implicit-function-declaration
	-Wno-implicit-int
	-Wno-conversion
	-Wno-sign-conversion
	-Wno-shorten-64-to-32
	-Wno-empty-body
	-Wno-misleading-indentation
	-Wno-unused-function
	-Wno-unused-variable
	-Wno-float-conversion
	-Wno-implicit-int-conversion
)

add_library(morph-config STATIC
	config/config.c
	config/schema.c
)
target_include_directories(morph-config PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/config
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

add_library(morph-credits STATIC
	credits.c
)
target_include_directories(morph-credits PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-credits PUBLIC morph-util morph-db morph-config morph-persistence)

add_subdirectory(render)
add_subdirectory(agent)
add_subdirectory(sandbox)
add_subdirectory(ext)
add_subdirectory(skill)
add_subdirectory(mcp)
add_subdirectory(ipc)
add_subdirectory(js_runner)
add_subdirectory(models)
add_subdirectory(http)
add_subdirectory(db)
add_subdirectory(persistence)
add_subdirectory(util)
add_subdirectory(event)
add_subdirectory(agent_ui)
add_subdirectory(sync)
add_subdirectory(runtime)
add_subdirectory(sapi)
