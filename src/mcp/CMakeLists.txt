add_library(morph-mcp STATIC
	mcp_client.c
	mcp_stdio.c
	mcp_http.c
)
target_include_directories(morph-mcp PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/vendor
)
target_compile_definitions(morph-mcp PUBLIC MORPH_VERSION="${PROJECT_VERSION}")
target_link_libraries(morph-mcp PUBLIC morph-util morph-http)
