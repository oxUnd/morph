cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(morph-tui-samples LANGUAGES C)
	set(CMAKE_C_STANDARD 11)
	set(CMAKE_C_STANDARD_REQUIRED ON)
	set(MORPH_TUI_SAMPLES_STANDALONE ON)
	find_package(Threads REQUIRED)
	find_package(PkgConfig REQUIRED)
	get_filename_component(MORPH_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

	# Select the terminal framework's utility closure from the shared manifest.
	# The full utility target also includes the shell parser and image helpers.
	set(TUI_SAMPLE_UTIL_MODULES
		array base64 buf cancel child_io error file id image_util log signals str
		terminal_size utf8 utf8proc
	)
	file(STRINGS "${MORPH_SOURCE_ROOT}/src/util/sources.list" TUI_SAMPLE_UTIL_MANIFEST)
	set(TUI_SAMPLE_UTIL_SOURCES)
	foreach(source IN LISTS TUI_SAMPLE_UTIL_MANIFEST)
		get_filename_component(module "${source}" NAME_WE)
		if(module IN_LIST TUI_SAMPLE_UTIL_MODULES)
			list(APPEND TUI_SAMPLE_UTIL_SOURCES "${MORPH_SOURCE_ROOT}/src/util/${source}")
		endif()
	endforeach()
	add_library(morph-util STATIC ${TUI_SAMPLE_UTIL_SOURCES})
	target_include_directories(morph-util PUBLIC
		"${MORPH_SOURCE_ROOT}/src"
		"${MORPH_SOURCE_ROOT}/src/util"
		"${MORPH_SOURCE_ROOT}/vendor"
	)
	target_compile_definitions(morph-util PRIVATE UTF8PROC_STATIC)
	target_link_libraries(morph-util PUBLIC Threads::Threads m)
	add_subdirectory("${MORPH_SOURCE_ROOT}/src/tui" "${CMAKE_CURRENT_BINARY_DIR}/tui")
endif()

add_subdirectory(minimal)
add_subdirectory(controls)
add_subdirectory(customization)
