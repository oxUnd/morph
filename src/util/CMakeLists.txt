add_library(multi-agent-util STATIC
	arena.c
	log.c
	file.c
)

target_include_directories(multi-agent-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})