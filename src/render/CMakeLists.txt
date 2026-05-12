add_library(multi-agent-render STATIC
	markdown.c
	image.c
	video.c
)

target_include_directories(multi-agent-render PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(multi-agent-render PUBLIC multi-agent-util md4c)