# Keep the pinned dependency free of unsigned negative-index arithmetic.
# Run after FetchContent, before compilation, including cached source trees.
function(morph_fix_cmark_gfm)
	FetchContent_GetProperties(cmark-gfm SOURCE_DIR cmark_source_dir)
	set(source_file "${cmark_source_dir}/extensions/autolink.c")
	file(READ "${source_file}" source_text)
	set(original "char prev_char = data[-((ptrdiff_t)rewind) - len - 1];")
	set(corrected "char prev_char = *(data - rewind - len - 1);")
	string(FIND "${source_text}" "${original}" original_offset)
	if(NOT original_offset EQUAL -1)
		string(REPLACE "${original}" "${corrected}" source_text "${source_text}")
		file(WRITE "${source_file}" "${source_text}")
	else()
		string(FIND "${source_text}" "${corrected}" corrected_offset)
		if(corrected_offset EQUAL -1)
			message(FATAL_ERROR "cmark-gfm autolink source changed; review the bounds fix")
		endif()
	endif()
endfunction()

function(morph_fix_cmark_inline_extensions)
	FetchContent_GetProperties(cmark-gfm SOURCE_DIR cmark_source_dir)
	set(source_file "${cmark_source_dir}/src/inlines.c")
	file(READ "${source_file}" source_text)
	# Give registered math extensions their backslash delimiters before the
	# ordinary Markdown escape handler consumes them. Other escapes fall back.
	set(original [=[  case '\\':
    new_inl = handle_backslash(parser, subj);
    break;]=])
	set(corrected [=[  case '\\':
    new_inl = try_extensions(parser, parent, c, subj);
    if (new_inl == NULL)
      new_inl = handle_backslash(parser, subj);
    break;]=])
	string(FIND "${source_text}" "${original}" original_offset)
	if(NOT original_offset EQUAL -1)
		string(REPLACE "${original}" "${corrected}" source_text "${source_text}")
		file(WRITE "${source_file}" "${source_text}")
	else()
		string(FIND "${source_text}" "${corrected}" corrected_offset)
		if(corrected_offset EQUAL -1)
			message(FATAL_ERROR "cmark-gfm inline source changed; review extension dispatch")
		endif()
	endif()
endfunction()

morph_fix_cmark_gfm()
morph_fix_cmark_inline_extensions()
