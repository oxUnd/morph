add_library(morph-tools STATIC
	text_gen.c
	text_qa.c
	img_gen.c
	img_edit.c
	img_info.c
	img_resize.c
	img_convert.c
	vid_gen.c
	file_read.c
	file_list.c
	file_info.c
	skill_activate.c
	bash_exec.c
	plan.c
	ask_user.c
	img_annotate.c
)

# stb_image_write/resize2 use long-form macros and pragmas; suppress noisy warnings
set_source_files_properties(img_resize.c img_convert.c PROPERTIES
	COMPILE_FLAGS "-Wno-sign-conversion -Wno-conversion -Wno-shadow -Wno-unused-but-set-variable -Wno-unused-function -Wno-double-promotion -Wno-cast-qual -Wno-missing-field-initializers"
)

target_include_directories(morph-tools PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/agent
	${CMAKE_SOURCE_DIR}/src/models
)
target_link_libraries(morph-tools PUBLIC morph-agent morph-models morph-http morph-util morph-skill morph-sandbox)
