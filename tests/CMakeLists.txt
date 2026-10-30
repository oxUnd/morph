include(GoogleTest)

find_package(Threads REQUIRED)

set(TEST_SOURCES
	test_arena.cpp
	test_hooks.cpp
	test_error.cpp
	error_macro_probe.c
	test_log.cpp
	test_state_ownership.cpp
	test_file.cpp
	test_database.cpp
	test_scheduled_task.cpp
	test_cli_setup.cpp
	test_config.cpp
	test_config_write.cpp
	test_credits.cpp
	test_session.cpp
	test_history_inspection.cpp
	test_memory.cpp
	test_runtime_query.cpp
	test_runtime_mcp.cpp
	test_runtime.cpp
	test_runtime_services.cpp
	test_session_title.cpp
	test_recap.cpp
	test_runtime_internal.cpp
	test_runtime_sync.cpp
	test_runtime_execute.cpp
	test_runtime_output.cpp
	test_dynamic_tools.cpp
	test_tool.cpp
	test_file_read.cpp
	test_file_list.cpp
	test_plan.cpp
	test_patch.cpp
	test_apply_patch_codex.cpp
	test_context.cpp
	test_environment_context.cpp
	test_project_context.cpp
	test_tokenizer.cpp
	test_model_tokens.cpp
	test_compress.cpp
	test_react.cpp
	test_agent_turn.cpp
	test_sse.cpp
	test_http.cpp
	test_image.cpp
	test_video.cpp
	test_skill.cpp
	test_sync.cpp
	test_bash_parse.cpp
	test_exec_process.cpp
	test_spin.cpp
	test_util_containers.cpp
	test_render.cpp
	test_mcp.cpp
	test_mcp_protocol.cpp
	test_ask_user.cpp
	test_sandbox.cpp
	test_tool_context.cpp
	test_request_permissions.cpp
	test_sub_agent.cpp
	test_ext.cpp
	test_event.cpp
	test_agent_ui.cpp
	test_cli_share.cpp
	test_cli_list.cpp
	test_cli_presentation.cpp
	test_cli_terminal.cpp
	test_tui.cpp
	test_cli_ui.cpp
	test_utf8.cpp
	test_kitty_protocol.cpp
	test_fastcgi_security.cpp
	test_fastcgi_runtime.cpp
	${CMAKE_SOURCE_DIR}/src/sapi/fastcgi/security.c
	${CMAKE_SOURCE_DIR}/src/sapi/fastcgi/agent_bridge.c
)

add_executable(morph-tests ${TEST_SOURCES})
add_library(morph-error-release-probe OBJECT error_macro_probe.c)
target_include_directories(morph-error-release-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(morph-error-release-probe PRIVATE -UDEBUG -DNDEBUG)
target_compile_definitions(morph-error-release-probe PRIVATE
	error_probe_return=error_release_probe_return
	error_probe_set=error_release_probe_set
	error_probe_errno=error_release_probe_errno
	error_probe_without_context=error_release_probe_without_context
	error_probe_wide=error_release_probe_wide
	error_probe_local_name=error_release_probe_local_name
	error_probe_assignment=error_release_probe_assignment)
target_sources(morph-tests PRIVATE $<TARGET_OBJECTS:morph-error-release-probe>)
set_target_properties(morph-tests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)
add_dependencies(morph-tests morph-data)

target_include_directories(morph-tests PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/util
	${CMAKE_SOURCE_DIR}/src/db
	${CMAKE_SOURCE_DIR}/src/agent
	${CMAKE_SOURCE_DIR}/src/agent/tools
	${CMAKE_SOURCE_DIR}/src/sandbox
	${CMAKE_SOURCE_DIR}/src/ext
	${CMAKE_SOURCE_DIR}/src/ipc
	${CMAKE_SOURCE_DIR}/src/models
	${CMAKE_SOURCE_DIR}/src/http
	${CMAKE_SOURCE_DIR}/src/render
	${CMAKE_SOURCE_DIR}/src/skill
	${CMAKE_SOURCE_DIR}/src/mcp
	${CMAKE_SOURCE_DIR}/src/event
	${CMAKE_SOURCE_DIR}/src/agent_ui
	${CMAKE_SOURCE_DIR}/src/sapi/fastcgi
)

target_compile_definitions(morph-tests PRIVATE
	MORPH_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(morph-tests PRIVATE
	morph-cli
	GTest::gtest_main
)

if(APPLE)
	target_link_libraries(morph-tests PRIVATE "-framework IOSurface" "-framework CoreFoundation")
endif()

gtest_discover_tests(morph-tests)

# PTY integration driver uses the production CLI with a disposable database.
add_executable(morph-cli-pty-driver cli_pty_driver.c)
target_include_directories(morph-cli-pty-driver PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morph-cli-pty-driver PRIVATE morph-cli)
set_target_properties(morph-cli-pty-driver PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)

add_executable(morph-setup-pty-driver cli_setup_pty_driver.c)
target_link_libraries(morph-setup-pty-driver PRIVATE morph-cli)
set_target_properties(morph-setup-pty-driver PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)

add_executable(morph-file-write-tests
	${CMAKE_SOURCE_DIR}/exts/file-write/test_file_write.c
	${CMAKE_SOURCE_DIR}/exts/file-write/file_write.c)
target_include_directories(morph-file-write-tests PRIVATE
	${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/vendor)
target_link_libraries(morph-file-write-tests PRIVATE morph-tools)
set_target_properties(morph-file-write-tests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
add_test(NAME file_write_extension COMMAND morph-file-write-tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(ENABLE_GLOBAL_STATE_AUDIT)
	if(NOT Python3_Interpreter_FOUND)
		message(FATAL_ERROR "Global state audit requires Python 3")
	endif()
	add_test(NAME global_state_audit
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_SOURCE_DIR}/scripts/audit_global_state.py
			--build-dir ${CMAKE_BINARY_DIR})
	set_tests_properties(global_state_audit PROPERTIES TIMEOUT 600)
	add_test(NAME global_state_audit_selftest
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_global_state_audit.py)
endif()
if(Python3_Interpreter_FOUND AND UNIX)
	add_test(NAME cli_snapshot
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_snapshot.py)
	set_tests_properties(cli_snapshot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 10)
	add_test(NAME macos_mcp_adapter
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_macos_mcp_adapter.py)
	add_test(NAME cli_manage_integration
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_cli_manage.py
			$<TARGET_FILE:morph>)
	set_tests_properties(cli_manage_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 120)
	add_test(NAME cli_options_integration
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_cli_options.py
			$<TARGET_FILE:morph>)
	add_test(NAME cli_setup_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_setup_pty.py
			$<TARGET_FILE:morph-setup-pty-driver> $<TARGET_FILE:morph>)
	set_tests_properties(cli_setup_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()
if(Python3_Interpreter_FOUND AND UNIX AND ENABLE_READLINE AND MORPH_READLINE_LIBRARY)
	add_test(NAME cli_mcp_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_mcp_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_mcp_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 30)
	add_test(NAME cli_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	add_test(NAME cli_share_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_share_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_share_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 45)
	add_test(NAME cli_tree_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_tree_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_tree_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 30)
	add_test(NAME cli_slash_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_slash_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_slash_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 45)
	add_test(NAME cli_completion_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_completion_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_completion_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 60)
	add_test(NAME cli_hooks_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_hooks_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_hooks_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	add_test(NAME cli_hooks_startup_pty_integration
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test_cli_hooks_startup_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_hooks_startup_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 45)
	add_test(NAME cli_resume_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_resume_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_resume_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	add_test(NAME cli_transcript_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_transcript_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_transcript_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	add_test(NAME cli_history_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_history_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_history_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	add_test(NAME cli_image_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_image_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	add_test(NAME cli_memory_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_memory_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	add_test(NAME cli_reasoning_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_reasoning_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_reasoning_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 60)
	add_test(NAME cli_jobs_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_jobs_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_jobs_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	add_test(NAME cli_recap_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_recap_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_recap_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	add_test(NAME cli_empty_session_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_empty_session_pty.py
			$<TARGET_FILE:morph>)
	set_tests_properties(cli_empty_session_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	set_tests_properties(cli_memory_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	set_tests_properties(cli_image_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	set_tests_properties(cli_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()
