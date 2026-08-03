add_library(morph-cli STATIC
	cancel_input.c
	core.c
	events.c
	init.c
	list_ui.c
	once.c
	output.c
	presentation.c
	run.c
	scheduler.c
	commands/ext.c
	commands/mcp.c
	commands/media.c
	commands/memory.c
	commands/permissions.c
	commands/registry.c
	commands/runtime.c
	commands/session.c
	commands/skill.c
	commands/sync.c
	commands/tasks.c
)
target_include_directories(morph-cli PUBLIC
	${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(morph-cli PUBLIC MORPH_VERSION="${PROJECT_VERSION}")
target_link_libraries(morph-cli PUBLIC
	morph-runtime
	morph-util
	morph-db
	morph-agent
	morph-http
	morph-models
	morph-config
	morph-session
	morph-credits
	morph-render
	MorphMarkdown::Kitty
	morph-skill
	morph-tools
	morph-ext
	morph-mcp
	morph-ipc
	morph-sandbox
	morph-event
	morph-agent-ui
	morph-sync
)
add_executable(morph main.c)
set_target_properties(morph PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)
target_include_directories(morph PRIVATE
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph PRIVATE morph-cli)
add_dependencies(morph morph-data)

install(TARGETS morph
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

find_path(READLINE_INCLUDE_DIR readline/readline.h
	/opt/homebrew/opt/readline/include
	/usr/local/opt/readline/include
	/usr/include
)
find_library(READLINE_LIBRARY readline
	/opt/homebrew/opt/readline/lib
	/usr/local/opt/readline/lib
	/usr/lib
)
if(READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
	target_include_directories(morph-cli PRIVATE ${READLINE_INCLUDE_DIR})
	target_compile_definitions(morph-cli PRIVATE HAVE_READLINE=1)
	target_link_libraries(morph-cli PUBLIC ${READLINE_LIBRARY})
	message(STATUS "readline found: ${READLINE_LIBRARY}")
else()
	message(STATUS "readline not found, using fgets")
endif()
