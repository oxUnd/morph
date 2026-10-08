set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_UTIL_SOURCES)
add_library(morph-util STATIC ${MORPH_UTIL_SOURCES})

target_include_directories(morph-util PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/vendor
)

target_link_libraries(morph-util PUBLIC m tree-sitter-bash)

set_source_files_properties(${CMAKE_SOURCE_DIR}/vendor/re.c PROPERTIES
	COMPILE_FLAGS "-Wno-sign-conversion -Wno-conversion"
)

set_source_files_properties(image_util.c PROPERTIES
	COMPILE_FLAGS "-Wno-sign-conversion -Wno-conversion -Wno-deprecated-declarations -Wno-unused-parameter -Wno-unused-but-set-variable"
)
