add_library(morph-render STATIC
	image.c
	video.c
)

target_include_directories(morph-render PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(morph-render PUBLIC
	nbase::nbase morph-cjson
	morph-media
	morph-terminal
	MorphMarkdown::KittyProtocol
)
