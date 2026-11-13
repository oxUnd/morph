add_library(morph-cjson STATIC cJSON.c)
target_include_directories(morph-cjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(APPLE)
    target_compile_options(morph-cjson PRIVATE -Wno-deprecated-declarations)
endif()

add_library(morph-regex STATIC re.c)
target_include_directories(morph-regex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(morph-regex PRIVATE -Wno-sign-conversion -Wno-conversion)
