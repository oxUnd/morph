add_executable(morph-tui-controls main.c)
target_link_libraries(morph-tui-controls PRIVATE morph-tui)
target_compile_options(morph-tui-controls PRIVATE
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion
)
set_target_properties(morph-tui-controls PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
