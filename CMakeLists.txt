cmake_minimum_required(VERSION 3.20)
project(morph LANGUAGES C CXX VERSION 0.3.8)

include(GNUInstallDirs)

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
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)
pkg_check_modules(VIPS REQUIRED vips)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)
pkg_check_modules(LIBUV REQUIRED libuv)

include(FetchContent)

if(POLICY CMP0169)
	cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_Declare(
	quickjs
	URL https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Populate(quickjs)

FetchContent_Declare(
	wasm3
	URL https://github.com/wasm3/wasm3/archive/refs/tags/v0.5.0.tar.gz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Populate(wasm3)
set(BUILD_WASI "none" CACHE STRING "Wasm3 WASI implementation" FORCE)
add_subdirectory(${wasm3_SOURCE_DIR}/source ${wasm3_BINARY_DIR}/source
	EXCLUDE_FROM_ALL)

FetchContent_Declare(
	blake3
	URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.8.5.tar.gz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Populate(blake3)

FetchContent_Declare(
	tree_sitter
	URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.25.10.tar.gz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Populate(tree_sitter)

FetchContent_Declare(
	tree_sitter_bash
	URL https://github.com/tree-sitter/tree-sitter-bash/archive/refs/tags/v0.25.1.tar.gz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Populate(tree_sitter_bash)

add_library(tree-sitter-core STATIC
	${tree_sitter_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(tree-sitter-core
	PUBLIC ${tree_sitter_SOURCE_DIR}/lib/include
	PRIVATE ${tree_sitter_SOURCE_DIR}/lib/src
)
target_compile_options(tree-sitter-core PRIVATE
	-Wno-conversion
	-Wno-sign-conversion
	-Wno-shadow
	-Wno-unused-parameter
	-Wno-pedantic
)

add_library(tree-sitter-bash STATIC
	${tree_sitter_bash_SOURCE_DIR}/src/parser.c
	${tree_sitter_bash_SOURCE_DIR}/src/scanner.c
)
target_include_directories(tree-sitter-bash
	PUBLIC ${tree_sitter_bash_SOURCE_DIR}/bindings/c
	PRIVATE ${tree_sitter_bash_SOURCE_DIR}/src
)
target_compile_options(tree-sitter-bash PRIVATE
	-Wno-conversion
	-Wno-sign-conversion
	-Wno-shadow
	-Wno-unused-parameter
	-Wno-unused-function
	-Wno-pedantic
)
target_link_libraries(tree-sitter-bash PUBLIC tree-sitter-core)

add_library(blake3 STATIC
	${blake3_SOURCE_DIR}/c/blake3.c
	${blake3_SOURCE_DIR}/c/blake3_dispatch.c
	${blake3_SOURCE_DIR}/c/blake3_portable.c
)
target_include_directories(blake3 PUBLIC
	${blake3_SOURCE_DIR}/c
)
target_compile_definitions(blake3 PRIVATE
	BLAKE3_NO_SSE2
	BLAKE3_NO_SSE41
	BLAKE3_NO_AVX2
	BLAKE3_NO_AVX512
	BLAKE3_USE_NEON=0
)

add_library(quickjs STATIC
	${quickjs_SOURCE_DIR}/quickjs.c
	${quickjs_SOURCE_DIR}/libregexp.c
	${quickjs_SOURCE_DIR}/libunicode.c
	${quickjs_SOURCE_DIR}/cutils.c
	${quickjs_SOURCE_DIR}/quickjs-libc.c
	${quickjs_SOURCE_DIR}/dtoa.c
)
target_include_directories(quickjs PUBLIC
	${quickjs_SOURCE_DIR}
)
target_compile_definitions(quickjs PRIVATE
	_GNU_SOURCE
	CONFIG_VERSION="2026-06-04"
)
target_compile_options(quickjs PRIVATE
	-Wno-conversion
	-Wno-sign-conversion
	-Wno-shadow
	-Wno-unused-parameter
	-Wno-unused-function
	-Wno-implicit-fallthrough
	-Wno-pedantic
)
target_link_libraries(quickjs PUBLIC m pthread)
if(NOT APPLE)
	target_link_libraries(quickjs PUBLIC dl)
endif()

add_library(mathjax STATIC
        vendor/mathjax-c/src/api.c
        vendor/mathjax-c/src/ast/node.c
        vendor/mathjax-c/src/ast/factory.c
        vendor/mathjax-c/src/ast/visitor.c
        vendor/mathjax-c/src/font/font.c
        vendor/mathjax-c/src/font/metrics.c
        vendor/mathjax-c/src/font/stix_data.c
        vendor/mathjax-c/src/render/bitmap.c
        vendor/mathjax-c/src/render/draw.c
        vendor/mathjax-c/src/kitty/kitty.c
        vendor/mathjax-c/src/layout/box.c
        vendor/mathjax-c/src/layout/math.c
        vendor/mathjax-c/src/layout/scripts.c
        vendor/mathjax-c/src/layout/fractions.c
        vendor/mathjax-c/src/layout/radicals.c
        vendor/mathjax-c/src/layout/matrix.c
        vendor/mathjax-c/src/layout/spacing.c
        vendor/mathjax-c/src/parser/lexer.c
        vendor/mathjax-c/src/parser/parser.c
        vendor/mathjax-c/src/parser/macros.c
        vendor/mathjax-c/src/parser/symbols.c
        vendor/mathjax-c/src/parser/environments.c
)
target_include_directories(mathjax PUBLIC
        ${CMAKE_SOURCE_DIR}/vendor/mathjax-c/include
        ${CMAKE_SOURCE_DIR}/vendor/mathjax-c/src/third_party
        ${FREETYPE_INCLUDE_DIRS}
        ${HARFBUZZ_INCLUDE_DIRS}
)
target_link_directories(mathjax PUBLIC
        ${FREETYPE_LIBRARY_DIRS}
        ${HARFBUZZ_LIBRARY_DIRS}
)
target_link_libraries(mathjax PUBLIC
        ${FREETYPE_LIBRARIES}
        ${HARFBUZZ_LIBRARIES}
        m
)
if(APPLE)
        find_library(CORE_TEXT CoreText)
        find_library(CORE_GRAPHICS CoreGraphics)
        if(CORE_TEXT AND CORE_GRAPHICS)
                target_link_libraries(mathjax PRIVATE
                        ${CORE_TEXT}
                        ${CORE_GRAPHICS}
                )
        endif()
endif()

set(MORPH_MARKDOWN_BUILD_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
set(MORPH_MARKDOWN_BUILD_KITTY ON CACHE BOOL "" FORCE)
set(MORPH_MARKDOWN_BUILD_KITTY_DEMO OFF CACHE BOOL "" FORCE)
add_subdirectory(fronts/morph-markdown)

add_compile_options(
	$<$<COMPILE_LANGUAGE:C>:-Wall>
	$<$<COMPILE_LANGUAGE:C>:-Wextra>
	$<$<COMPILE_LANGUAGE:C>:-Wpedantic>
	$<$<COMPILE_LANGUAGE:C>:-Wshadow>
	$<$<COMPILE_LANGUAGE:C>:-Wconversion>
	$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-gnu-zero-variadic-macro-arguments>
)

add_custom_target(morph-data ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/fonts
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/tiktoken
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/skills
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_SOURCE_DIR}/vendor/mathjax-c/fonts/STIXTwoMath-Regular.ttf
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/fonts/
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_SOURCE_DIR}/vendor/tiktoken/cl100k_base.tiktoken
		${CMAKE_SOURCE_DIR}/vendor/tiktoken/o200k_base.tiktoken
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/tiktoken/
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_SOURCE_DIR}/docs/morph.txt
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/morph.txt
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_SOURCE_DIR}/skills
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/skills
	COMMAND ${CMAKE_COMMAND} -E rm -f
		${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/morph/skills/.DS_Store
	COMMENT "Staging morph runtime data"
)

add_subdirectory(src)

install(FILES
	${CMAKE_SOURCE_DIR}/vendor/tiktoken/cl100k_base.tiktoken
	${CMAKE_SOURCE_DIR}/vendor/tiktoken/o200k_base.tiktoken
	DESTINATION ${CMAKE_INSTALL_DATADIR}/morph/tiktoken
)
install(FILES
	${CMAKE_SOURCE_DIR}/vendor/mathjax-c/fonts/STIXTwoMath-Regular.ttf
	DESTINATION ${CMAKE_INSTALL_DATADIR}/morph/fonts
)
install(FILES
	${CMAKE_SOURCE_DIR}/docs/morph.txt
	DESTINATION ${CMAKE_INSTALL_DATADIR}/morph
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/skills/
	DESTINATION ${CMAKE_INSTALL_DATADIR}/morph/skills
	PATTERN ".DS_Store" EXCLUDE
)
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
