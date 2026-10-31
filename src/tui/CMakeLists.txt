pkg_check_modules(TUI_LIBUV REQUIRED IMPORTED_TARGET libuv>=1.44)

add_library(morph-tui STATIC
	application.c input.c terminal.c frame.c draw.c
	layout.c list.c select.c select_prompt.c editor.c card.c text_view.c screen.c tree.c
)
target_include_directories(morph-tui PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(morph-tui PUBLIC morph-util PRIVATE PkgConfig::TUI_LIBUV)
