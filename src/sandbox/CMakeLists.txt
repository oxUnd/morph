add_library(morph-sandbox STATIC
	sandbox.c
	loader.c
)

target_include_directories(morph-sandbox PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/skill
)
target_link_libraries(morph-sandbox PUBLIC morph-util ${CMAKE_DL_LIBS})

if(APPLE)
	find_library(SECURITY_FRAMEWORK Security)
	find_library(SANDBOX_LIBRARY sandbox)
	if(SANDBOX_LIBRARY)
		target_link_libraries(morph-sandbox PUBLIC ${SANDBOX_LIBRARY})
	endif()
endif()
