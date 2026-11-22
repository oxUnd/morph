find_package(Git REQUIRED)

function(morph_apply_rendering_patch label source patch_name)
	get_filename_component(source "${source}" REALPATH)
	get_filename_component(parent "${source}" DIRECTORY)
	set(patch "${CMAKE_CURRENT_SOURCE_DIR}/vendor/patches/${patch_name}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
	set(git_command "${CMAKE_COMMAND}" -E env
		--unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_COMMON_DIR --unset=GIT_PREFIX
		"GIT_CEILING_DIRECTORIES=${parent}" "${GIT_EXECUTABLE}"
		-c "safe.directory=${source}" -C "${source}")
	execute_process(COMMAND ${git_command} apply --reverse --check "${patch}"
		RESULT_VARIABLE applied OUTPUT_QUIET ERROR_VARIABLE reverse_error)
	if(applied EQUAL 0)
		return()
	endif()

	execute_process(COMMAND ${git_command} apply --check "${patch}"
		RESULT_VARIABLE applicable OUTPUT_QUIET ERROR_VARIABLE forward_error)
	if(NOT applicable EQUAL 0)
		message(FATAL_ERROR "${label} patch does not match the source. Preserve local "
			"changes and review ${patch}.\nForward: ${forward_error}\nReverse: ${reverse_error}")
	endif()

	execute_process(COMMAND ${git_command} apply "${patch}"
		RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE apply_error)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "Could not apply ${label} patch: ${apply_error}")
	endif()
endfunction()
