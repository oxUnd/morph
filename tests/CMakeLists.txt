include(GoogleTest)

find_package(Threads REQUIRED)

set(TEST_SOURCES
	test_arena.cpp
	test_error.cpp
	test_log.cpp
	test_file.cpp
	test_database.cpp
	test_scheduled_task.cpp
	test_config.cpp
	test_config_write.cpp
	test_credits.cpp
	test_session.cpp
	test_memory.cpp
	test_runtime_query.cpp
	test_runtime.cpp
	test_runtime_services.cpp
	test_runtime_internal.cpp
	test_runtime_sync.cpp
	test_runtime_execute.cpp
	test_dynamic_tools.cpp
	test_tool.cpp
	test_plan.cpp
	test_context.cpp
	test_tokenizer.cpp
	test_compress.cpp
	test_react.cpp
	test_agent_turn.cpp
	test_sse.cpp
	test_http.cpp
	test_image.cpp
	test_skill.cpp
	test_sync.cpp
	test_markdown.cpp
	test_bash_exec.cpp
	test_spin.cpp
	test_util_containers.cpp
	test_render.cpp
	test_mcp.cpp
	test_ask_user.cpp
	test_sandbox.cpp
	test_tool_context.cpp
	test_sub_agent.cpp
	test_ext.cpp
	test_event.cpp
	test_agent_ui.cpp
	test_fastcgi_security.cpp
	${CMAKE_SOURCE_DIR}/src/sapi/fastcgi/security.c
)

add_executable(morph-tests ${TEST_SOURCES})

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

gtest_discover_tests(morph-tests)
