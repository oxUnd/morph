add_library(morph-skill STATIC
	skill.c
	manifest.c
)

target_include_directories(morph-skill PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/agent
)
target_link_libraries(morph-skill PUBLIC morph-util morph-sandbox morph-toml)
