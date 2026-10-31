add_executable(morph-tui-minimal main.c)
target_link_libraries(morph-tui-minimal PRIVATE morph-tui)
target_compile_options(morph-tui-minimal PRIVATE
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion
)
set_target_properties(morph-tui-minimal PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
