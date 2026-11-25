add_library(morph-db STATIC
	database.c
	schema.c
	schema_models.c
	error_migration.c
	permission_grant.c
	scheduled_task.c
	records.c
	history_models.c
	session_models.c
	memory_models.c
	session_sync_audit.c
)

target_include_directories(morph-db PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-db PUBLIC nbase::nbase morph-cjson
	PRIVATE nothing_orm::core)

add_executable(morph-session-sync-audit
	${CMAKE_SOURCE_DIR}/src/diagnostics/session_sync_audit_main.c)
target_link_libraries(morph-session-sync-audit PRIVATE morph-db)
set_target_properties(morph-session-sync-audit PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
install(TARGETS morph-session-sync-audit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
