add_library(morph-sync STATIC
	db_backup.c
	sync.c
)

target_include_directories(morph-sync PUBLIC
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(morph-sync PUBLIC
	morph-util
	blake3
	SQLite3::SQLite3
	${LIBUV_LIBRARIES}
)

target_include_directories(morph-sync PUBLIC
	${LIBUV_INCLUDE_DIRS}
)

target_link_directories(morph-sync PUBLIC
	${LIBUV_LIBRARY_DIRS}
)
