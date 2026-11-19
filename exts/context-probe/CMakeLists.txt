cmake_minimum_required(VERSION 3.20)
project(morph-context-probe C)

set(MORPH_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Matching Morph checkout")
set(MORPH_SDK_INCLUDE_DIR "${MORPH_SOURCE_DIR}/include" CACHE PATH "Exported Morph ext SDK headers")
set(NBASE_INCLUDE_DIR "${MORPH_SOURCE_DIR}/build/_deps/nbase-src/include"
	CACHE PATH "Matching nbase public headers")

add_library(context-probe MODULE probe.c "${MORPH_SOURCE_DIR}/vendor/cJSON.c")
target_include_directories(context-probe PRIVATE
	"${MORPH_SDK_INCLUDE_DIR}" "${MORPH_SOURCE_DIR}/vendor" "${NBASE_INCLUDE_DIR}")
target_compile_features(context-probe PRIVATE c_std_11)
target_compile_definitions(context-probe PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(context-probe PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
set_target_properties(context-probe PROPERTIES PREFIX "" OUTPUT_NAME "probe" SUFFIX ".so")
configure_file(manifest.toml manifest.toml COPYONLY)
