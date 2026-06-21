add_library(morph-event STATIC
	event.c
)

target_include_directories(morph-event PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(morph-event PUBLIC morph-util)
