find_package(Git REQUIRED)

set(_morph_tui_patch
	"${CMAKE_CURRENT_LIST_DIR}/../vendor/patches/nothing-tui-update-convergence.patch")
file(REAL_PATH "${nothing_tui_SOURCE_DIR}" _morph_tui_source)
get_filename_component(_morph_tui_parent "${_morph_tui_source}" DIRECTORY)
set(_morph_tui_git "${CMAKE_COMMAND}" -E env
	--unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_COMMON_DIR --unset=GIT_PREFIX
	"GIT_CEILING_DIRECTORIES=${_morph_tui_parent}"
	"${GIT_EXECUTABLE}" -C "${_morph_tui_source}")
execute_process(
	COMMAND ${_morph_tui_git} apply --reverse --check "${_morph_tui_patch}"
	RESULT_VARIABLE _morph_tui_applied OUTPUT_QUIET ERROR_QUIET)
if(NOT _morph_tui_applied EQUAL 0)
	execute_process(
		COMMAND ${_morph_tui_git} apply --check "${_morph_tui_patch}"
		RESULT_VARIABLE _morph_tui_applicable
		OUTPUT_QUIET ERROR_VARIABLE _morph_tui_error)
	if(NOT _morph_tui_applicable EQUAL 0)
		message(FATAL_ERROR "nothing-tui convergence patch does not match the source. "
			"Preserve local changes and review "
			"vendor/patches/nothing-tui-update-convergence.patch.\n${_morph_tui_error}")
	endif()
	execute_process(
		COMMAND ${_morph_tui_git} apply "${_morph_tui_patch}"
		COMMAND_ERROR_IS_FATAL ANY)
endif()
