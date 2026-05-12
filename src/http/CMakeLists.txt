add_library(multi-agent-http STATIC
	client.c
	sse.c
)

target_include_directories(multi-agent-http PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-http PUBLIC multi-agent-util CURL::libcurl)