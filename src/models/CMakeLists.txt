add_library(multi-agent-models STATIC
	llm.c
	image_gen.c
	video_gen.c
)

target_include_directories(multi-agent-models PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-models PUBLIC multi-agent-util multi-agent-http)