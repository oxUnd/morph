set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_MODEL_SOURCES)
add_library(morph-models STATIC ${MORPH_MODEL_SOURCES})

target_include_directories(morph-models PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-models PUBLIC morph-util morph-http)
