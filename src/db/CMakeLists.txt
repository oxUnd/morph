add_library(morph-db STATIC
	database.c
)

target_include_directories(morph-db PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-db PUBLIC morph-util SQLite::SQLite3)
