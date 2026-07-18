add_subdirectory(cli)

if(BUILD_FASTCGI)
	add_subdirectory(fastcgi)
endif()
