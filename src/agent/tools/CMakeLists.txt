add_library(multi-agent-tools STATIC
	text_gen.c
	text_qa.c
	img_gen.c
	img_edit.c
	img_info.c
	vid_gen.c
)

target_include_directories(multi-agent-tools PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/agent
	${CMAKE_SOURCE_DIR}/src/models
)
target_link_libraries(multi-agent-tools PUBLIC multi-agent-agent multi-agent-models multi-agent-http multi-agent-util)