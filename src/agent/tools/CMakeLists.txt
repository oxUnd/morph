set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_TOOL_SOURCES)
add_library(morph-tools STATIC ${MORPH_TOOL_SOURCES})

# stb_image_write/resize2 use long-form macros and pragmas; suppress noisy warnings
set_source_files_properties(img_resize.c img_convert.c img_inpaint.c img_compose.c PROPERTIES
	COMPILE_FLAGS "-Wno-sign-conversion -Wno-conversion -Wno-shadow -Wno-unused-but-set-variable -Wno-unused-function -Wno-double-promotion -Wno-cast-qual -Wno-missing-field-initializers"
)

target_include_directories(morph-tools PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/agent
	${CMAKE_SOURCE_DIR}/src/models
)
target_link_libraries(morph-tools PUBLIC
	morph-agent morph-models morph-http nbase::nbase morph-cjson morph-skill
	morph-sandbox morph-db morph-credits morph-session morph-config
	morph-exec morph-command-analysis morph-app-data morph-media
)

if(UNIX)
	target_link_libraries(morph-tools PUBLIC m)
endif()
