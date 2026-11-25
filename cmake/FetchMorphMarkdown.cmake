include_guard(GLOBAL)
include(FetchContent)

set(MORPH_MARKDOWN_BUILD_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
set(MORPH_MARKDOWN_BUILD_KITTY ON CACHE BOOL "" FORCE)
set(MORPH_MARKDOWN_BUILD_KITTY_DEMO OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
	morph_markdown
	GIT_REPOSITORY https://github.com/oxUnd/morph-markdown.git
	GIT_TAG 785ad7fa38c7d61b0f8b41f1a9a9bc5ea6e213ea
)
FetchContent_MakeAvailable(morph_markdown)
