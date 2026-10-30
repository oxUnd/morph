add_library(morph-tui STATIC
	input.c terminal.c frame.c draw.c
	layout.c list.c select.c select_prompt.c editor.c card.c text_view.c screen.c tree.c
)
target_include_directories(morph-tui PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(morph-tui PUBLIC morph-util)
