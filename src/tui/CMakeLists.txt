pkg_check_modules(TUI_LIBUV REQUIRED IMPORTED_TARGET libuv>=1.44)

add_library(morph-tui STATIC
	core/application.c
	core/runner.c
	core/view.c
	core/view_controller.c
	core/window.c
	core/layout.c
	core/screen.c

	terminal/input.c
	terminal/terminal.c

	render/draw.c
	render/canvas.c
	render/media.c
	render/frame.c
	render/native_media.c

	widgets/card.c
	widgets/image.c
	widgets/base.c
	widgets/label.c
	widgets/stack.c
	widgets/scroll.c
	widgets/list_view.c
	widgets/text_field.c
	widgets/editor.c
	widgets/list.c
	widgets/select.c
	widgets/select_prompt.c
	widgets/text_view.c
	widgets/document.c
	widgets/tree.c
)
target_include_directories(morph-tui PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(morph-tui PUBLIC morph-util PRIVATE PkgConfig::TUI_LIBUV)

option(BUILD_TUI_SAMPLES "Build terminal UI examples" OFF)
if(BUILD_TUI_SAMPLES AND NOT MORPH_TUI_SAMPLES_STANDALONE)
	add_subdirectory(samples)
endif()
