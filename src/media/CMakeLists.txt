add_library(morph-media STATIC image.c)
target_include_directories(morph-media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(MORPH_MEDIA_VENDOR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor" ABSOLUTE)
target_include_directories(morph-media PRIVATE ${MORPH_MEDIA_VENDOR})
target_link_libraries(morph-media PUBLIC nbase::nbase)
set_source_files_properties(image.c PROPERTIES
    COMPILE_FLAGS "-Wno-sign-conversion -Wno-conversion -Wno-deprecated-declarations -Wno-unused-parameter -Wno-unused-but-set-variable")
