add_executable(morph-tui-customization
	main.c
	application.c
	widgets/counter.c
	pages/dashboard.c
	pages/inspector.c
	pages/help.c
)
target_include_directories(morph-tui-customization PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(morph-tui-customization PRIVATE morph-tui)
target_compile_options(morph-tui-customization PRIVATE
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion
)
set_target_properties(morph-tui-customization PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
