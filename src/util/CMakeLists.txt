add_library(multi-agent-util STATIC
	arena.c
	log.c
	file.c
	base64.c
	image_util.c
	${CMAKE_SOURCE_DIR}/vendor/cJSON.c
)

target_include_directories(multi-agent-util PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/vendor
)