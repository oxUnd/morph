cmake_minimum_required(VERSION 3.20)
project(morph C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS "-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-gnu-zero-variadic-macro-arguments")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

option(BUILD_TESTS "Build tests" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

if(ENABLE_ASAN)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer")
	set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address -fsanitize=undefined")
endif()

find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

include(FetchContent)

FetchContent_Declare(
	md4c
	URL      https://github.com/mity/md4c/archive/refs/tags/release-0.5.3.tar.gz
)
FetchContent_MakeAvailable(md4c)

add_subdirectory(src)

if(BUILD_TESTS)
	enable_testing()
	FetchContent_Declare(
		googletest
		GIT_REPOSITORY https://github.com/google/googletest.git
		GIT_TAG        v1.14.0
	)
	FetchContent_MakeAvailable(googletest)
	add_subdirectory(tests)
endif()