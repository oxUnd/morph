pkg_check_modules(TUI_LIBUV REQUIRED IMPORTED_TARGET libuv>=1.44)

add_library(morph-tui STATIC
	core/application.c
	core/layout.c
	core/screen.c

	terminal/input.c
	terminal/terminal.c

	render/draw.c
	render/frame.c

	widgets/card.c
	widgets/editor.c
	widgets/list.c
	widgets/select.c
	widgets/select_prompt.c
	widgets/text_view.c
	widgets/tree.c
)
target_include_directories(morph-tui PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(morph-tui PUBLIC morph-util PRIVATE PkgConfig::TUI_LIBUV)
