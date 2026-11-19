// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include <string>
#include <string_view>

#include "EditorEngine.h"

/*
	Doom3Editor.exe: the engine hosted in the editor's own window.

	  Doom3Editor.exe [+engine commands]          the Qt interface (editor\Doom3EditorUI.dll)
	  Doom3Editor.exe --bare [+engine commands]   the viewport alone, in a plain frame
*/

namespace {

// the editor's own switches start with "--" and are not for the engine's command line
[[nodiscard]] bool TakeSwitch( std::string & cmdLine, std::string_view name ) {
	const size_t at = cmdLine.find( name );
	if ( at == std::string::npos ) {
		return false;
	}
	cmdLine.erase( at, name.size() );
	return true;
}

}	// namespace

int WINAPI WinMain( _In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR cmdLine, _In_ int ) {
	std::string engineCmdLine = cmdLine;
	const bool bare = TakeSwitch( engineCmdLine, "--bare" );

	// One thread: nothing of a frame is pending between two frames, so the editor can change the world there.
	// A window inside the host's, whatever the saved video mode says.
	engineCmdLine += " +set com_smp 0 +set r_fullscreen 0";

	return bare ? Editor_RunBare( instance, engineCmdLine.c_str() ) : Editor_RunUi( instance, engineCmdLine.c_str() );
}
