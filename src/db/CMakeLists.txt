add_library(morph-db STATIC
	database.c
	error_migration.c
	permission_grant.c
	scheduled_task.c
)

target_include_directories(morph-db PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-db PUBLIC nbase::nbase morph-cjson SQLite3::SQLite3)
