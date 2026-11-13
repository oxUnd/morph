cmake_minimum_required(VERSION 3.20)
project(morph_read_lines LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(NB_BUILD_SHARED OFF CACHE BOOL "Link the extension to static nbase" FORCE)
set(NB_BUILD_TESTS OFF CACHE BOOL "Build nbase tests" FORCE)
set(NB_BUILD_EXAMPLES OFF CACHE BOOL "Build nbase examples" FORCE)
set(NB_WERROR ON CACHE BOOL "Treat nbase warnings as errors" FORCE)
add_subdirectory(../../vendor/nothing nbase)

add_library(read_lines MODULE read_lines.c)
target_link_libraries(read_lines PRIVATE nbase::nbase)
target_compile_definitions(read_lines PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(read_lines PRIVATE
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
set_target_properties(read_lines PROPERTIES PREFIX "" SUFFIX ".so")

add_executable(read_lines_test_driver test_driver.c read_lines.c)
target_link_libraries(read_lines_test_driver PRIVATE nbase::nbase)
target_compile_definitions(read_lines_test_driver PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(read_lines_test_driver PRIVATE
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(NB_SANITIZE)
	add_test(NAME read_lines_extension COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_read_lines.py
		$<TARGET_FILE:read_lines_test_driver> --probe)
else()
	add_test(NAME read_lines_extension COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/test_read_lines.py $<TARGET_FILE:read_lines>)
endif()
