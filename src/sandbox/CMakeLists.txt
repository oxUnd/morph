add_library(morph-sandbox STATIC
	sandbox.c
	loader.c
)

target_include_directories(morph-sandbox PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-sandbox PUBLIC morph-util)
