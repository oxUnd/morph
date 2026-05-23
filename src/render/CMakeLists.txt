add_library(morph-render STATIC
	markdown.c
	image.c
	video.c
)

target_include_directories(morph-render PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(morph-render PRIVATE _XOPEN_SOURCE=700)

target_link_libraries(morph-render PUBLIC morph-util md4c)
