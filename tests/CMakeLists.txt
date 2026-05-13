include(GoogleTest)

find_package(Threads REQUIRED)

set(TEST_SOURCES
	test_arena.cpp
	test_log.cpp
	test_file.cpp
	test_database.cpp
	test_config.cpp
	test_session.cpp
	test_tool.cpp
	test_context.cpp
	test_tokenizer.cpp
	test_text_gen.cpp
	test_compress.cpp
	test_react.cpp
	test_sse.cpp
	test_http.cpp
	test_image.cpp
)

add_executable(multi-agent-tests ${TEST_SOURCES})

target_include_directories(multi-agent-tests PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/util
	${CMAKE_SOURCE_DIR}/src/db
	${CMAKE_SOURCE_DIR}/src/agent
	${CMAKE_SOURCE_DIR}/src/agent/tools
	${CMAKE_SOURCE_DIR}/src/sandbox
	${CMAKE_SOURCE_DIR}/src/skill
	${CMAKE_SOURCE_DIR}/src/ipc
	${CMAKE_SOURCE_DIR}/src/models
	${CMAKE_SOURCE_DIR}/src/http
	${CMAKE_SOURCE_DIR}/src/render
)

target_link_libraries(multi-agent-tests PRIVATE
	multi-agent-util
	multi-agent-db
	multi-agent-agent
	multi-agent-http
	multi-agent-sandbox
	multi-agent-skill
	multi-agent-ipc
	multi-agent-models
	multi-agent-render
	multi-agent-config
	multi-agent-session
	multi-agent-tools
	GTest::gtest_main
	SQLite::SQLite3
	CURL::libcurl
	 Threads::Threads
)

gtest_discover_tests(multi-agent-tests)