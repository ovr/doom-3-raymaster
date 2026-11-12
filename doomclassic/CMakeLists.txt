# Classic Doom / Doom II (DoomClassicCommon.props)

include(sources.cmake)

function(d3bfg_doomclassic_common tgt)
	target_compile_definitions(${tgt} PRIVATE
		_D3XP
		CTF
		_CRT_SECURE_NO_DEPRECATE
		WIN32
		_LIB
		UNICODE
		_UNICODE
	)
	target_include_directories(${tgt} PRIVATE "${D3BFG_NEO_DIR}")
	d3bfg_configure_target(${tgt} WARNING_LEVEL 3)
	set_target_properties(${tgt} PROPERTIES FOLDER "doomclassic")
endfunction()

add_library(doomclassic STATIC ${DOOMCLASSIC_SOURCES})
d3bfg_doomclassic_common(doomclassic)

add_library(timidity STATIC ${TIMIDITY_SOURCES})
d3bfg_doomclassic_common(timidity)
