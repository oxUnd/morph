set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_PERMISSION_SOURCES)
add_library(morph-permission STATIC ${MORPH_PERMISSION_SOURCES})
target_include_directories(morph-permission PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morph-permission PUBLIC morph-util morph-db Threads::Threads)
