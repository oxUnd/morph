add_library(morph-sync STATIC
	db_backup.c
	db_restore.c
	content_hash.c
	sync.c
	../persistence/orm/sync_manifest_models.c
	../persistence/orm/sync_manifest.c
	../persistence/orm/file_database.c
)

target_include_directories(morph-sync PUBLIC
	${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(morph-sync PUBLIC
	nbase::nbase morph-cjson
	blake3
	morph-db
	PRIVATE nothing_orm::sqlite
)
