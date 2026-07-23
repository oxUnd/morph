add_library(morph-models STATIC
	llm/llm.c
	image_gen.c
	image_openai.c
	image_volcengine.c
	video_gen.c
)

target_include_directories(morph-models PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-models PUBLIC morph-util morph-http)
