find_package(Git REQUIRED)

set(_morph_mathjax_source "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mathjax-c")
set(_morph_mathjax_patch
	"${CMAKE_CURRENT_SOURCE_DIR}/vendor/patches/mathjax-parser-ownership.patch")
get_filename_component(_morph_mathjax_source "${_morph_mathjax_source}" REALPATH)
get_filename_component(_morph_mathjax_parent "${_morph_mathjax_source}" DIRECTORY)
get_filename_component(_morph_mathjax_parent "${_morph_mathjax_parent}" REALPATH)
# An archive without its own .git must not inherit Morph's repository prefix:
# git apply can otherwise silently skip every hunk and report a successful check.
set(_morph_mathjax_git
	"${CMAKE_COMMAND}" -E env
	--unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_COMMON_DIR --unset=GIT_PREFIX
	"GIT_CEILING_DIRECTORIES=${_morph_mathjax_parent}"
	"${GIT_EXECUTABLE}" -c "safe.directory=${_morph_mathjax_source}"
	-C "${_morph_mathjax_source}")
execute_process(
	COMMAND ${_morph_mathjax_git} apply --reverse --check "${_morph_mathjax_patch}"
	RESULT_VARIABLE _morph_mathjax_applied
	OUTPUT_QUIET ERROR_VARIABLE _morph_mathjax_reverse_error)
if(NOT _morph_mathjax_applied EQUAL 0)
	execute_process(
		COMMAND ${_morph_mathjax_git} apply --check "${_morph_mathjax_patch}"
		RESULT_VARIABLE _morph_mathjax_applicable
		OUTPUT_QUIET ERROR_VARIABLE _morph_mathjax_forward_error)
	if(NOT _morph_mathjax_applicable EQUAL 0)
		message(FATAL_ERROR "MathJax ownership patch does not match the source. "
			"Preserve local changes and review vendor/patches/mathjax-parser-ownership.patch.\n"
			"Forward check: ${_morph_mathjax_forward_error}\n"
			"Reverse check: ${_morph_mathjax_reverse_error}")
	endif()
	execute_process(
		COMMAND ${_morph_mathjax_git} apply "${_morph_mathjax_patch}"
		RESULT_VARIABLE _morph_mathjax_patched
		OUTPUT_QUIET ERROR_VARIABLE _morph_mathjax_apply_error)
	if(NOT _morph_mathjax_patched EQUAL 0)
		message(FATAL_ERROR "Could not apply MathJax ownership patch: "
			"${_morph_mathjax_apply_error}")
	endif()
endif()
