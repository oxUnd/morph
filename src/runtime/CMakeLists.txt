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
)
