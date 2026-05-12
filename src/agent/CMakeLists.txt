add_library(multi-agent-agent STATIC
	react.c
	context.c
	compress.c
	tokenizer.c
	tool.c
)

target_include_directories(multi-agent-agent PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-agent PUBLIC multi-agent-util)

add_subdirectory(tools)