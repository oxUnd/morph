include_guard(GLOBAL)
include(FetchContent)

set(NORM_BUILD_SHARED OFF)
set(NORM_BUILD_TESTS OFF)
set(NORM_BUILD_EXAMPLES OFF)
set(NORM_BUILD_API_REVIEW OFF)
set(NORM_BUILD_TEST_SUPPORT ${BUILD_TESTS})
set(NORM_WITH_SQLITE ON)
set(NORM_WITH_MYSQL OFF)
set(NORM_WITH_POSTGRESQL OFF)
set(NORM_WITH_PGVECTOR OFF)

# Reuse the nbase target configured by FetchNbase.cmake.
FetchContent_Declare(
	nothing_orm
	GIT_REPOSITORY https://codeberg.org/oxund/nothing-orm.git
	GIT_TAG 243633b2deb3550b7fb091950bf88d8e781b37d5
	SOURCE_SUBDIR include
)
FetchContent_MakeAvailable(nothing_orm)

# Keep structured query/schema and controlled SQLite extensions reproducible.
find_package(Git REQUIRED)
set(nothing_orm_patch
	"${CMAKE_CURRENT_LIST_DIR}/../vendor/patches/nothing-orm-sqlite-migrations.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nothing_orm_patch}")
file(REAL_PATH "${nothing_orm_SOURCE_DIR}" nothing_orm_checkout)
get_filename_component(nothing_orm_parent "${nothing_orm_checkout}" DIRECTORY)
set(nothing_orm_git "${CMAKE_COMMAND}" -E env
	--unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_COMMON_DIR --unset=GIT_PREFIX
	"GIT_CEILING_DIRECTORIES=${nothing_orm_parent}"
	"${GIT_EXECUTABLE}" -C "${nothing_orm_checkout}")
execute_process(
	COMMAND ${nothing_orm_git} apply --reverse --check "${nothing_orm_patch}"
	RESULT_VARIABLE nothing_orm_patch_applied OUTPUT_QUIET ERROR_QUIET)
if(NOT nothing_orm_patch_applied EQUAL 0)
	execute_process(
		COMMAND ${nothing_orm_git} apply "${nothing_orm_patch}"
		COMMAND_ERROR_IS_FATAL ANY)
endif()

add_subdirectory("${nothing_orm_SOURCE_DIR}" "${nothing_orm_BINARY_DIR}" EXCLUDE_FROM_ALL)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	# The pinned driver passes validated enum values to signed PRAGMA arguments.
	set_property(SOURCE "${nothing_orm_SOURCE_DIR}/src/driver/sqlite.c"
		TARGET_DIRECTORY nothing_orm
		APPEND PROPERTY COMPILE_OPTIONS -Wno-sign-conversion)
endif()
