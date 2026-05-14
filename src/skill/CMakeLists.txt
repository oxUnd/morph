add_library(morph-skill STATIC
	skill.c
	skill_parse.c
)

target_include_directories(morph-skill PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-skill PUBLIC morph-util)
