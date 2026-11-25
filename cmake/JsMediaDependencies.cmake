# Keep the runner's image stack together, including its transitive flags.
function(morph_find_js_media_dependencies)
	set(pkg_config_options IMPORTED_TARGET GLOBAL)
	if(APPLE)
		find_program(MORPH_BREW_EXECUTABLE brew
			HINTS /opt/homebrew/bin /usr/local/bin)
		if(NOT MORPH_BREW_EXECUTABLE)
			message(FATAL_ERROR
				"Homebrew is required for libvips on macOS. Install Homebrew, then run: brew install vips pkgconf")
		endif()

		execute_process(COMMAND "${MORPH_BREW_EXECUTABLE}" --prefix
			OUTPUT_VARIABLE brew_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
			COMMAND_ERROR_IS_FATAL ANY)
		execute_process(COMMAND "${MORPH_BREW_EXECUTABLE}" --prefix vips
			OUTPUT_VARIABLE vips_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
			COMMAND_ERROR_IS_FATAL ANY)
		set(PKG_CONFIG_EXECUTABLE "${brew_prefix}/opt/pkgconf/bin/pkg-config")
		if(NOT EXISTS "${vips_prefix}/lib/pkgconfig/vips.pc" OR
		   NOT EXISTS "${PKG_CONFIG_EXECUTABLE}")
			message(FATAL_ERROR
				"Homebrew libvips/pkgconf is missing. Run: brew install vips pkgconf")
		endif()

		# Keg-only dependencies (notably libarchive) are absent from the
		# default search path. Query Homebrew rather than maintaining a list.
		execute_process(COMMAND "${MORPH_BREW_EXECUTABLE}" deps --installed --formula vips
			OUTPUT_VARIABLE brew_dependencies OUTPUT_STRIP_TRAILING_WHITESPACE
			COMMAND_ERROR_IS_FATAL ANY)
		string(REPLACE "\n" ";" brew_dependencies "${brew_dependencies}")
		set(pkg_config_paths "${vips_prefix}/lib/pkgconfig")
		foreach(dependency IN LISTS brew_dependencies)
			list(APPEND pkg_config_paths
				"${brew_prefix}/opt/${dependency}/lib/pkgconfig"
				"${brew_prefix}/opt/${dependency}/share/pkgconfig")
		endforeach()

		# Retain Homebrew's SDK metadata for system libraries such as libffi
		# and zlib, while excluding other package managers and caller paths.
		execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=pc_path pkg-config
			OUTPUT_VARIABLE default_pkg_config_path OUTPUT_STRIP_TRAILING_WHITESPACE
			COMMAND_ERROR_IS_FATAL ANY)
		string(REPLACE ":" ";" default_pkg_config_paths "${default_pkg_config_path}")
		foreach(path IN LISTS default_pkg_config_paths)
			string(FIND "${path}" "${brew_prefix}/" prefix_position)
			if(prefix_position EQUAL 0)
				list(APPEND pkg_config_paths "${path}")
			endif()
		endforeach()

		list(REMOVE_DUPLICATES pkg_config_paths)
		list(JOIN pkg_config_paths ":" pkg_config_path)
		foreach(variable PKG_CONFIG_PATH PKG_CONFIG_LIBDIR)
			set(saved_${variable} "$ENV{${variable}}")
			set(had_${variable} FALSE)
			if(DEFINED ENV{${variable}})
				set(had_${variable} TRUE)
			endif()
			set(ENV{${variable}} "${pkg_config_path}")
		endforeach()

		# Resolve .pc prefixes through stable opt links, not versioned Cellar
		# directories that disappear after brew upgrade/cleanup.
		set(PKG_CONFIG_ARGN --define-prefix)
		list(APPEND pkg_config_options NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH)
		find_package(PkgConfig REQUIRED)
		message(STATUS "JS media dependencies: Homebrew (${vips_prefix})")
	endif()

	# FindPkgConfig can cache FOUND even when a later flags query failed.
	# Validate the complete graph and refresh cached flags/library paths on
	# every configure, including existing builds after a Homebrew upgrade.
	if(APPLE)
		execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}" ${PKG_CONFIG_ARGN}
			--cflags --libs vips cairo pangocairo
			RESULT_VARIABLE media_status OUTPUT_QUIET ERROR_VARIABLE media_error)
		if(NOT media_status EQUAL 0)
			message(FATAL_ERROR
				"Cannot resolve Homebrew JS media dependencies:\n${media_error}\nRun: brew reinstall vips")
		endif()
	endif()

	get_cmake_property(cache_variables CACHE_VARIABLES)
	foreach(variable IN LISTS cache_variables)
		if(variable MATCHES "^(__pkg_config_checked_|pkgcfg_lib_)(VIPS|CAIRO|PANGOCAIRO)(_|$)")
			unset(${variable} CACHE)
		endif()
	endforeach()

	pkg_check_modules(VIPS REQUIRED ${pkg_config_options} vips)
	pkg_check_modules(CAIRO REQUIRED ${pkg_config_options} cairo)
	pkg_check_modules(PANGOCAIRO REQUIRED ${pkg_config_options} pangocairo)

	if(APPLE)
		foreach(variable PKG_CONFIG_PATH PKG_CONFIG_LIBDIR)
			if(had_${variable})
				set(ENV{${variable}} "${saved_${variable}}")
			else()
				unset(ENV{${variable}})
			endif()
		endforeach()
	endif()
endfunction()
