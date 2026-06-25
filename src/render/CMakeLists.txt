add_library(morph-render STATIC
	markdown.c
	highlight.c
	image.c
	video.c
)

target_include_directories(morph-render PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(morph-render PRIVATE _XOPEN_SOURCE=700)
target_compile_definitions(morph-render PRIVATE
        MORPH_MATHJAX_FONT_PATH="${CMAKE_SOURCE_DIR}/vendor/mathjax-c/fonts/STIXTwoMath-Regular.ttf"
)

target_link_libraries(morph-render PUBLIC morph-util md4c mathjax)
