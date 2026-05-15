add_library(morph-agent STATIC
	react.c
	context.c
	compress.c
	tokenizer.c
	tool.c
)

target_include_directories(morph-agent PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-agent PUBLIC morph-util Threads::Threads)

add_subdirectory(tools)
