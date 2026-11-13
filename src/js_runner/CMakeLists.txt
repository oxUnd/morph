set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_JS_RUNNER_SOURCES)
add_executable(morph-js-runner ${MORPH_JS_RUNNER_SOURCES})

set_target_properties(morph-js-runner PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)
add_dependencies(morph-js-runner morph-data)

target_include_directories(morph-js-runner PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${VIPS_INCLUDE_DIRS}
	${CAIRO_INCLUDE_DIRS}
	${PANGOCAIRO_INCLUDE_DIRS}
)

target_link_directories(morph-js-runner PRIVATE
	${VIPS_LIBRARY_DIRS}
	${CAIRO_LIBRARY_DIRS}
	${PANGOCAIRO_LIBRARY_DIRS}
)

set(MORPH_JS_MEDIA_LIBRARIES
	${VIPS_LIBRARIES} ${CAIRO_LIBRARIES} ${PANGOCAIRO_LIBRARIES})
list(REMOVE_DUPLICATES MORPH_JS_MEDIA_LIBRARIES)

target_link_libraries(morph-js-runner PRIVATE
	quickjs
	morph-http
	nbase::nbase morph-cjson
	morph-media
	m3
	${MORPH_JS_MEDIA_LIBRARIES}
)

target_compile_options(morph-js-runner PRIVATE
	-Wno-unused-parameter
	-Wno-conversion
	-Wno-sign-conversion
	-Wno-shorten-64-to-32
	-Wno-pedantic
)

install(TARGETS morph-js-runner
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
