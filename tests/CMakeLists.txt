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
	test_skill.cpp
	test_markdown.cpp
	test_bash_exec.cpp
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
)

target_link_libraries(morph-tests PRIVATE
	morph-util
	morph-db
	morph-agent
	morph-http
	morph-sandbox
	morph-ext
	morph-ipc
	morph-models
	morph-render
	morph-config
	morph-session
	morph-tools
	morph-skill
	GTest::gtest_main
	SQLite::SQLite3
	CURL::libcurl
	 Threads::Threads
)

gtest_discover_tests(morph-tests)
