set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/sources.list")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" MORPH_AGENT_SOURCES)
add_library(morph-agent STATIC ${MORPH_AGENT_SOURCES})

target_include_directories(morph-agent PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-agent PUBLIC morph-util morph-db morph-session morph-persistence morph-models morph-http morph-event Threads::Threads)

add_subdirectory(tools)
