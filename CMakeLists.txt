cmake_minimum_required(VERSION 3.20)
project(morph LANGUAGES C CXX VERSION 0.3.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

option(BUILD_TESTS "Build tests" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(BUILD_FASTCGI "Build FastCGI front-end (morph-fastcgi)" OFF)

if(ENABLE_ASAN)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer")
	set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address -fsanitize=undefined")
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

include(FetchContent)

if(EXISTS "${CMAKE_SOURCE_DIR}/_deps/md4c-0.5.3.tar.gz")
	FetchContent_Declare(
		md4c
		URL "${CMAKE_SOURCE_DIR}/_deps/md4c-0.5.3.tar.gz"
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	)
else()
	FetchContent_Declare(
		md4c
		URL      https://github.com/mity/md4c/archive/refs/tags/release-0.5.3.tar.gz
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	)
endif()
FetchContent_MakeAvailable(md4c)

add_compile_options(
	$<$<COMPILE_LANGUAGE:C>:-Wall>
	$<$<COMPILE_LANGUAGE:C>:-Wextra>
	$<$<COMPILE_LANGUAGE:C>:-Wpedantic>
	$<$<COMPILE_LANGUAGE:C>:-Wshadow>
	$<$<COMPILE_LANGUAGE:C>:-Wconversion>
	$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-gnu-zero-variadic-macro-arguments>
)

configure_file(
	${CMAKE_SOURCE_DIR}/vendor/tiktoken/cl100k_base.tiktoken
	${CMAKE_BINARY_DIR}/tiktoken/cl100k_base.tiktoken COPYONLY)
configure_file(
	${CMAKE_SOURCE_DIR}/vendor/tiktoken/o200k_base.tiktoken
	${CMAKE_BINARY_DIR}/tiktoken/o200k_base.tiktoken COPYONLY)

add_subdirectory(src)

if(BUILD_TESTS)
	enable_testing()
	FetchContent_Declare(
		googletest
		GIT_REPOSITORY https://github.com/google/googletest.git
		GIT_TAG        v1.14.0
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	)
	FetchContent_MakeAvailable(googletest)
	add_subdirectory(tests)
endif()
