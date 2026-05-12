add_library(multi-agent-db STATIC
	database.c
)

target_include_directories(multi-agent-db PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(multi-agent-db PUBLIC multi-agent-util SQLite::SQLite3)