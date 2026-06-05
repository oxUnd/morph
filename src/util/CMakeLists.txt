add_library(morph-util STATIC
	arena.c
	log.c
	file.c
	base64.c
	image_util.c
	utf8.c
	spin.c
	error.c
	bpe.c
	${CMAKE_SOURCE_DIR}/vendor/cJSON.c
	${CMAKE_SOURCE_DIR}/vendor/re.c
)

target_include_directories(morph-util PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/vendor
)

target_compile_definitions(morph-util PUBLIC
	VENDOR_TIKTOKEN_DIR="${CMAKE_BINARY_DIR}/tiktoken"
)

target_link_libraries(morph-util PUBLIC m)

set_source_files_properties(${CMAKE_SOURCE_DIR}/vendor/re.c PROPERTIES
	COMPILE_FLAGS "-Wno-sign-conversion -Wno-conversion"
)
