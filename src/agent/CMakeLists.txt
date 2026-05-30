add_library(morph-agent STATIC
	react.c
	context.c
	compress.c
	tokenizer.c
	tool.c
	plan.c
	memory.c
	guardrail.c
)

target_include_directories(morph-agent PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-agent PUBLIC morph-util morph-db morph-models morph-http Threads::Threads)

add_subdirectory(tools)
