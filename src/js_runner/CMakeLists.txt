add_executable(morph-js-runner
	main.c
)

set_target_properties(morph-js-runner PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(morph-js-runner PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/util
)

target_link_libraries(morph-js-runner PRIVATE
	quickjs
	morph-util
)

target_compile_options(morph-js-runner PRIVATE
	-Wno-unused-parameter
	-Wno-conversion
	-Wno-sign-conversion
	-Wno-shorten-64-to-32
	-Wno-pedantic
)
