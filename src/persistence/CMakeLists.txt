set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_PERSISTENCE_SOURCES)
add_library(morph-persistence STATIC ${MORPH_PERSISTENCE_SOURCES})

target_include_directories(morph-persistence PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-persistence PUBLIC morph-util morph-db morph-session)
