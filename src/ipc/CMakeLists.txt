add_library(multi-agent-ipc STATIC
	jsonrpc.c
)

target_include_directories(multi-agent-ipc PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-ipc PUBLIC multi-agent-util)