add_library(morph-cli STATIC
	# Application
	application.c

	# Application models
	models/subagents.c
	models/composer.c
	models/agents.c
	models/jobs.c
	models/transcript.c

	# Page controllers
	pages/approval.c
	pages/chat.c
	pages/history.c
	pages/hooks.c
	pages/jobs.c
	pages/memory.c
	pages/subagents.c
	pages/model.c
	pages/permissions.c
	pages/question.c
	pages/resume.c
	pages/rewind.c
	presentation/turn_details.c
	pages/setup/controls.c
	pages/setup/wizard.c

	# Application widgets
	widgets/subagents.c
	widgets/agents.c
	widgets/agents_browser.c
	widgets/banner.c
	widgets/composer_panel.c
	widgets/feed.c
	widgets/feed_block.c
	widgets/jobs.c
	widgets/list.c
	widgets/notice.c
	widgets/notification.c
	widgets/plan.c
	widgets/shell_style.c
	widgets/transcript.c

	# Commands
	commands/agents.c
	commands/ext.c
	commands/hooks.c
	commands/input.c
	commands/jobs.c
	commands/mcp.c
	commands/mcp_manage.c
	commands/media.c
	commands/memory.c
	commands/options.c
	commands/permissions.c
	commands/registry.c
	commands/rewind.c
	commands/runtime.c
	commands/session.c
	commands/share.c
	commands/skill.c
	commands/skills_manage.c
	commands/sync.c
	commands/tasks.c

	# Application services
	services/agents.c
	services/composer.c
	services/command_job.c
	services/configuration/config.c
	services/configuration/mcp.c
	services/configuration/model.c
	services/configuration/role.c
	services/event_queue.c
	services/management.c
	services/recap.c
	services/scheduler.c
	services/session_restore.c
	services/share_store.c
	services/skill_store.c
	services/transcript.c
	services/turn.c

	# Platform and protocol adapters
	adapters/clipboard/clipboard.c
	adapters/input/callbacks.c
	adapters/input/cancel.c
	adapters/input/keymap.c
	adapters/input/reader.c
	adapters/input/readline.c
	adapters/input/state.c
	adapters/media/image_loader.c
	adapters/media/image_probe.c
	adapters/input/confirm.c
	adapters/media/renderer.c
	adapters/protocol/interaction.c
	adapters/runtime/events.c
	adapters/runtime/dispatch.c
	adapters/runtime/observer.c

	# Application presentation
	presentation/agent_tools.c
	presentation/about.c
	presentation/diagnostics.c
	presentation/history.c
	presentation/markdown.c
	presentation/messages.c
	presentation/notifications.c
	presentation/output.c
	presentation/recap.c
	presentation/command_output.c
	presentation/confirm.c
	presentation/agents.c
	presentation/jobs.c
	presentation/presenter.c
	presentation/rewind.c
	presentation/tool_preview.c
	presentation/share.c

)
if(NOT TARGET morph-markdown-html)
	message(FATAL_ERROR "morph CLI requires MORPH_MARKDOWN_BUILD_HTML=ON")
endif()
add_dependencies(morph-cli morph-data)
target_include_directories(morph-cli PUBLIC
	${CMAKE_SOURCE_DIR}/src
)
pkg_check_modules(CLI_WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(CLI_FFI REQUIRED IMPORTED_TARGET libffi)
target_link_libraries(morph-cli PRIVATE PkgConfig::CLI_FFI)
target_link_libraries(morph-cli PUBLIC PkgConfig::CLI_WEBP PRIVATE CURL::libcurl)
target_compile_definitions(morph-cli PUBLIC MORPH_VERSION="${PROJECT_VERSION}")
target_link_libraries(morph-cli PUBLIC
	morph-runtime
	morph-util
	morph-tui
	morph-db
	morph-agent
	morph-http
	morph-models
	morph-config
	morph-session
	morph-credits
	morph-render
	MorphMarkdown::Kitty
	morph-markdown-html
	morph-skill
	morph-tools
	morph-ext
	morph-mcp
	morph-ipc
	morph-sandbox
	morph-event
	morph-agent-ui
	morph-sync
	blake3
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

if(ENABLE_READLINE)
	find_path(MORPH_READLINE_INCLUDE_DIR readline/readline.h
		/opt/homebrew/opt/readline/include
		/usr/local/opt/readline/include
		/usr/include
		NO_DEFAULT_PATH
	)
	find_library(MORPH_READLINE_LIBRARY readline
		/opt/homebrew/opt/readline/lib
		/usr/local/opt/readline/lib
		/usr/lib
		NO_DEFAULT_PATH
	)
	if(NOT MORPH_READLINE_INCLUDE_DIR)
		find_path(MORPH_READLINE_INCLUDE_DIR readline/readline.h)
	endif()
	if(NOT MORPH_READLINE_LIBRARY)
		find_library(MORPH_READLINE_LIBRARY readline)
	endif()
endif()
if(ENABLE_READLINE AND MORPH_READLINE_INCLUDE_DIR AND MORPH_READLINE_LIBRARY)
	include(CheckCSourceCompiles)
	set(CMAKE_REQUIRED_INCLUDES ${MORPH_READLINE_INCLUDE_DIR})
	set(CMAKE_REQUIRED_LIBRARIES ${MORPH_READLINE_LIBRARY})
	check_c_source_compiles([=[
#include <stdio.h>
#include <readline/readline.h>
int main(void)
{
	return rl_bind_keyseq("", 0);
}
]=] MORPH_HAVE_RL_BIND_KEYSEQ)
	check_c_source_compiles([=[
#include <stdio.h>
#include <readline/readline.h>
int main(void) { return rl_bracketed_paste_begin(1, 0); }
]=] MORPH_HAVE_RL_BRACKETED_PASTE_BEGIN)
	unset(CMAKE_REQUIRED_INCLUDES)
	unset(CMAKE_REQUIRED_LIBRARIES)
	target_include_directories(morph-cli PRIVATE ${MORPH_READLINE_INCLUDE_DIR})
	target_compile_definitions(morph-cli PRIVATE HAVE_READLINE=1)
	if(MORPH_HAVE_RL_BRACKETED_PASTE_BEGIN)
		target_compile_definitions(morph-cli PRIVATE HAVE_RL_BRACKETED_PASTE_BEGIN=1)
	endif()
	if(MORPH_HAVE_RL_BIND_KEYSEQ)
		target_compile_definitions(morph-cli PRIVATE HAVE_RL_BIND_KEYSEQ=1)
	endif()
	target_link_libraries(morph-cli PUBLIC ${MORPH_READLINE_LIBRARY})
	message(STATUS "readline found: ${MORPH_READLINE_LIBRARY}")
elseif(ENABLE_READLINE)
	message(STATUS "readline not found, using fgets")
else()
	message(STATUS "readline disabled, using fgets")
endif()
