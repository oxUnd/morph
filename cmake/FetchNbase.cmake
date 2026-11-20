include(FetchContent)

if(NOT TARGET nbase::nbase)
	set(NB_BUILD_SHARED OFF)
	set(NB_BUILD_TESTS OFF)
	set(NB_BUILD_EXAMPLES OFF)
	set(NB_BUILD_BENCHMARKS OFF)
	set(NB_BUILD_FUZZERS OFF)
	# Morph uses nbase for HTTP, including streaming and multipart uploads.
	set(NB_WITH_HTTP ON)

	# Populate the source first so we can exclude the dependency's install rules.
	FetchContent_Declare(
		nbase
		GIT_REPOSITORY https://codeberg.org/oxund/nothing.git
		GIT_TAG fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3
		SOURCE_SUBDIR include
	)
	FetchContent_MakeAvailable(nbase)

	if(CYGWIN)
		find_package(Git REQUIRED)
		set(nbase_patch
			"${CMAKE_CURRENT_LIST_DIR}/../vendor/patches/nbase-cygwin-executable-path.patch")
		file(REAL_PATH "${nbase_SOURCE_DIR}" nbase_checkout)
		get_filename_component(nbase_parent "${nbase_checkout}" DIRECTORY)
		set(nbase_git "${CMAKE_COMMAND}" -E env
			--unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_COMMON_DIR --unset=GIT_PREFIX
			"GIT_CEILING_DIRECTORIES=${nbase_parent}"
			"${GIT_EXECUTABLE}" -C "${nbase_checkout}")
		execute_process(
			COMMAND ${nbase_git} apply --reverse --check "${nbase_patch}"
			RESULT_VARIABLE nbase_patch_applied OUTPUT_QUIET ERROR_QUIET)
		if(NOT nbase_patch_applied EQUAL 0)
			execute_process(
				COMMAND ${nbase_git} apply "${nbase_patch}"
				COMMAND_ERROR_IS_FATAL ANY)
		endif()
	endif()

	add_subdirectory("${nbase_SOURCE_DIR}" "${nbase_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
