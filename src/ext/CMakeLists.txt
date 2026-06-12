add_library(morph-ext STATIC
	ext.c
	install.c
	manifest.c
)

target_include_directories(morph-ext PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/agent
)
target_link_libraries(morph-ext PUBLIC morph-util morph-sandbox morph-toml)
