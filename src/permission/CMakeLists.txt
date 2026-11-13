add_library(morph-command-analysis STATIC command_analysis.c)
target_include_directories(morph-command-analysis PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morph-command-analysis PUBLIC nbase::nbase tree-sitter-bash)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_PERMISSION_SOURCES)
add_library(morph-permission STATIC ${MORPH_PERMISSION_SOURCES})
target_include_directories(morph-permission PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morph-permission PUBLIC
	nbase::nbase morph-cjson morph-command-analysis morph-db Threads::Threads)
