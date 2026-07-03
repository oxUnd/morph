add_executable(morph-js-runner
	main.c
	js_media.c
)

set_target_properties(morph-js-runner PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(morph-js-runner PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/util
	${VIPS_INCLUDE_DIRS}
	${CAIRO_INCLUDE_DIRS}
	${PANGOCAIRO_INCLUDE_DIRS}
)

target_link_directories(morph-js-runner PRIVATE
	${VIPS_LIBRARY_DIRS}
	${CAIRO_LIBRARY_DIRS}
	${PANGOCAIRO_LIBRARY_DIRS}
)

target_link_libraries(morph-js-runner PRIVATE
	quickjs
	morph-util
	m3
	${VIPS_LIBRARIES}
	${CAIRO_LIBRARIES}
	${PANGOCAIRO_LIBRARIES}
)

target_compile_options(morph-js-runner PRIVATE
	-Wno-unused-parameter
	-Wno-conversion
	-Wno-sign-conversion
	-Wno-shorten-64-to-32
	-Wno-pedantic
)
