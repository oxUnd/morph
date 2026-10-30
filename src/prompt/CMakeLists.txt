add_library(morph-prompt STATIC template.c)
target_include_directories(morph-prompt PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morph-prompt PUBLIC morph-util)
