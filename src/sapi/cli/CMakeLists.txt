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
	pages/turn_details.c
	pages/setup/controls.c
	pages/setup/wizard.c

	# Application components
	components/setup_prompt.c
	components/subagents.c
	components/agents.c
	components/agents_browser.c
	components/banner.c
	components/skill_tree.c
	components/selector.c
	components/composer.c
	components/message.c
	components/mcp_tree.c
	components/media.c
	components/rich_document.c
	components/tool_call.c
	components/feed.c
	components/feed_block.c
	components/jobs.c
	components/list.c
	components/visibility.c
	components/notice.c
	components/notification.c
	components/plan.c
	components/recap.c
	components/report.c
	components/shell_style.c
	components/transcript.c

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
	adapters/input/reader.c
	adapters/input/state.c
	adapters/media/asset.c
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
	presentation/mcp_tree.c
	presentation/notifications.c
	presentation/output.c
	presentation/recap.c
	presentation/command_output.c
	presentation/composer_footer.c
	presentation/confirm.c
	presentation/agents.c
	presentation/jobs.c
	presentation/presenter.c
	presentation/rewind.c
	presentation/tool_preview.c
	presentation/work_status.c
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
	nbase::nbase morph-cjson
	nothing-tui::nothing-tui
	morph-terminal
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
	morph-app-data
	morph-media
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
if(CYGWIN)
	# The configuration includes path arrays larger than Cygwin's 2 MiB stack.
	target_link_options(morph PRIVATE "LINKER:--stack,33554432")
endif()

install(TARGETS morph
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

target_link_libraries(morph-cli PRIVATE PkgConfig::NT_READLINE)
