add_library(morph-sync STATIC
	db_backup.c
	db_restore.c
	content_hash.c
	sync.c
)

target_include_directories(morph-sync PUBLIC
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(morph-sync PUBLIC
	nbase::nbase morph-cjson
	blake3
	SQLite3::SQLite3
)
