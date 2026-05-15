add_library(morph-util STATIC
	arena.c
	log.c
	file.c
	base64.c
	image_util.c
	utf8.c
	${CMAKE_SOURCE_DIR}/vendor/cJSON.c
)

target_include_directories(morph-util PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/vendor
)
