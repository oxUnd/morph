add_library(morph-agent-ui STATIC
	markdown.c
	tags.c
)

target_include_directories(morph-agent-ui PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(morph-agent-ui PUBLIC morph-util)
