include_guard(GLOBAL)
include(FetchContent)

set(NORM_BUILD_SHARED OFF)
set(NORM_BUILD_TESTS OFF)
set(NORM_BUILD_EXAMPLES OFF)
set(NORM_BUILD_API_REVIEW OFF)
set(NORM_BUILD_TEST_SUPPORT ${BUILD_TESTS})
set(NORM_WITH_SQLITE ON)
set(NORM_WITH_MYSQL OFF)
set(NORM_WITH_POSTGRESQL ${ENABLE_POSTGRESQL})
set(NORM_WITH_PGVECTOR OFF)

# Reuse the nbase target configured by FetchNbase.cmake.
FetchContent_Declare(
	nothing_orm
	GIT_REPOSITORY https://codeberg.org/oxund/nothing-orm.git
	GIT_TAG 46115ce4b1cae9e1a9b68f52b1f8ebd85c4779a3
	SOURCE_SUBDIR include
)
FetchContent_MakeAvailable(nothing_orm)

add_subdirectory("${nothing_orm_SOURCE_DIR}" "${nothing_orm_BINARY_DIR}" EXCLUDE_FROM_ALL)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	# The pinned driver passes validated enum values to signed PRAGMA arguments.
	set_property(SOURCE "${nothing_orm_SOURCE_DIR}/src/driver/sqlite.c"
		TARGET_DIRECTORY nothing_orm
		APPEND PROPERTY COMPILE_OPTIONS -Wno-sign-conversion)
endif()
