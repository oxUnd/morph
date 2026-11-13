add_library(morph-app-data STATIC resources.c)
target_include_directories(morph-app-data PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morph-app-data PUBLIC nbase::nbase)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

add_library(morph-runtime STATIC
	${MORPH_RUNTIME_SOURCE_NAMES}
)
target_include_directories(morph-runtime PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-runtime PUBLIC
	morph-agent
	morph-config
	morph-credits
	morph-event
	morph-mcp
	morph-session
	morph-skill
	morph-sync
	morph-tools
	morph-ext
	morph-ipc
	morph-app-data
)
