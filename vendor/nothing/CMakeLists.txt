cmake_minimum_required(VERSION 3.20)

project(nbase VERSION 0.1.0 DESCRIPTION "Small C base library" LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
  add_compile_definitions(_DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
else()
  # MinGW's ANSI stdio implementation supports C99 formats such as %zu.
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

include(GNUInstallDirs)

option(NB_BUILD_SHARED "Build nbase as a shared library" OFF)
option(NB_BUILD_TESTS "Build nbase tests" ON)
option(NB_BUILD_EXAMPLES "Build nbase examples" ON)
option(NB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(NB_WERROR "Treat warnings as errors" OFF)
option(NB_BUILD_BENCHMARKS "Build standalone performance benchmarks" OFF)
option(NB_BUILD_FUZZERS "Build libFuzzer parser checks (Clang + NB_SANITIZE)" OFF)

set(NB_SOURCES
    src/base.c
    src/cancel.c
    src/err.c
    src/mem.c
    src/str.c
    src/buf.c
    src/vec.c
    src/map.c
    src/time.c
    src/log.c
    src/log_file.c
    src/path.c
    src/file.c
    src/process.c
    src/poll.c
    src/random.c
    src/encoding.c
    src/hash.c
    src/ini.c
    src/utf8.c
    src/unicode.c
    vendor/utf8proc/utf8proc.c
    src/toml.c
)

if(WIN32)
  list(APPEND NB_SOURCES
      src/platform/win32/file.c
      src/platform/win32/path.c
      src/platform/win32/process.c
      src/platform/win32/poll.c
      src/platform/win32/random.c
      src/platform/win32/io.c
      src/platform/win32/thread.c
      src/platform/win32/signal.c
      src/platform/win32/socket.c
      src/platform/win32/time.c)
else()
  list(APPEND NB_SOURCES
      src/platform/posix/file.c
      src/platform/posix/path.c
      src/platform/posix/process.c
      src/platform/posix/poll.c
      src/platform/posix/random.c
      src/platform/posix/io.c
      src/platform/posix/thread.c
      src/platform/posix/signal.c
      src/platform/posix/socket.c
      src/platform/posix/time.c)
endif()

if(NB_BUILD_SHARED)
  add_library(nbase SHARED ${NB_SOURCES})
  target_compile_definitions(nbase PRIVATE NB_BUILDING_DLL INTERFACE NB_USING_DLL)
else()
  add_library(nbase STATIC ${NB_SOURCES})
endif()
add_library(nbase::nbase ALIAS nbase)
target_compile_definitions(nbase PRIVATE UTF8PROC_STATIC)

target_include_directories(nbase
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/utf8proc
)

if(WIN32)
  target_compile_definitions(nbase PRIVATE
      _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(nbase PUBLIC ws2_32 bcrypt)
else()
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(nbase PUBLIC Threads::Threads)
endif()

if(MSVC)
  target_compile_options(nbase PRIVATE /W4)
else()
  target_compile_options(nbase PRIVATE -Wall -Wextra -Wpedantic)
  if(NB_WERROR)
    target_compile_options(nbase PRIVATE -Werror)
  endif()
endif()

if(MSVC AND NB_WERROR)
  target_compile_options(nbase PRIVATE /WX)
endif()

if(NB_BUILD_BENCHMARKS)
  add_executable(nbase_bench benchmarks/bench.c)
  target_link_libraries(nbase_bench PRIVATE nbase)
  add_executable(nbase_bench_text benchmarks/bench_text.c)
  target_link_libraries(nbase_bench_text PRIVATE nbase)
endif()

if(NB_SANITIZE)
  target_compile_options(nbase PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(nbase PUBLIC -fsanitize=address,undefined)
endif()

if(NB_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT NB_SANITIZE)
    message(FATAL_ERROR "NB_BUILD_FUZZERS requires Clang and NB_SANITIZE=ON")
  endif()
  target_compile_options(nbase PRIVATE -fsanitize=fuzzer-no-link)
  add_executable(nbase_fuzz_text tests/fuzz_text.c)
  target_link_libraries(nbase_fuzz_text PRIVATE nbase)
  target_compile_options(nbase_fuzz_text PRIVATE -fsanitize=fuzzer)
  target_link_options(nbase_fuzz_text PRIVATE -fsanitize=fuzzer)
endif()

install(TARGETS nbase
  EXPORT nbaseTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/nb DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT nbaseTargets
  FILE nbaseTargets.cmake
  NAMESPACE nbase::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nbase
)

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/nbaseConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/nbaseConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nbase)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/nbaseConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nbaseConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/nbaseConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nbase)

if(WIN32)
  set(NB_PC_PRIVATE_LIBS "-lws2_32 -lbcrypt")
else()
  set(NB_PC_PRIVATE_LIBS "-pthread")
endif()

configure_file(nbase.pc.in nbase.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nbase.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

if(NB_BUILD_TESTS)
  enable_testing()
  add_executable(nbase_tests
    tests/test_main.c
    tests/test_core.c
    tests/test_system.c
    tests/test_containers.c
    tests/test_text.c
    tests/test_config.c
    tests/test_file_time.c
    tests/test_runtime.c
    tests/test_utf8.c
    tests/test_hash.c
    tests/test_regressions.c
    tests/test_toml.c
    tests/test_process.c
    tests/test_fs_extra.c
    tests/test_core_extra.c
    tests/test_signal.c
    tests/test_log_file.c
  )
  target_link_libraries(nbase_tests PRIVATE nbase)
  if(MSVC)
    target_compile_options(nbase_tests PRIVATE /W4)
  else()
    target_compile_options(nbase_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(NB_WERROR)
      target_compile_options(nbase_tests PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME nbase_tests COMMAND nbase_tests)

  add_executable(nbase_toml_corpus tests/test_toml_corpus.c)
  target_link_libraries(nbase_toml_corpus PRIVATE nbase)
  if(MSVC)
    target_compile_options(nbase_toml_corpus PRIVATE /W4)
  else()
    target_compile_options(nbase_toml_corpus PRIVATE
      -Wall -Wextra -Wpedantic)
    if(NB_WERROR)
      target_compile_options(nbase_toml_corpus PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME nbase_toml_corpus
    COMMAND nbase_toml_corpus ${CMAKE_CURRENT_SOURCE_DIR}/tests/toml-test)
endif()

if(NB_BUILD_EXAMPLES)
  set(NB_EXAMPLES
    basic
    base
    err
    mem
    str
    buf
    vec
    map
    log
    time
    file
    encoding
    ini
    toml
    case_log_index
    utf8
    hash
    thread
    socket
  )
  foreach(name IN LISTS NB_EXAMPLES)
    add_executable(nbase_example_${name} examples/${name}.c)
    target_link_libraries(nbase_example_${name} PRIVATE nbase)
    if(MSVC)
      target_compile_options(nbase_example_${name} PRIVATE /W4)
    else()
      target_compile_options(nbase_example_${name} PRIVATE
        -Wall -Wextra -Wpedantic)
      if(NB_WERROR)
        target_compile_options(nbase_example_${name} PRIVATE -Werror)
      endif()
    endif()
    if(NB_BUILD_TESTS)
      add_test(NAME nbase_example_${name} COMMAND nbase_example_${name})
    endif()
  endforeach()
endif()
