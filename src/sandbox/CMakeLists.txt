add_library(multi-agent-sandbox STATIC
	sandbox.c
	loader.c
)

target_include_directories(multi-agent-sandbox PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-sandbox PUBLIC multi-agent-util)