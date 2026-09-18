add_library(morph-exec STATIC
	command_analyzer.c
	process.c
)

target_include_directories(morph-exec PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(morph-exec PUBLIC
	morph-util
	morph-sandbox
)
