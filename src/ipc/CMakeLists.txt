add_library(morph-ipc STATIC
	jsonrpc.c
)

target_include_directories(morph-ipc PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-ipc PUBLIC morph-util)
