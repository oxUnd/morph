add_library(morph-http STATIC request.c)

target_include_directories(morph-http PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-http PUBLIC nbase::nbase)
