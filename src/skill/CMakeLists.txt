add_library(multi-agent-skill STATIC
	skill.c
	manifest.c
)

target_include_directories(multi-agent-skill PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/agent
)
target_link_libraries(multi-agent-skill PUBLIC multi-agent-util multi-agent-sandbox)