/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __SYS_WINDOW_H__
#define __SYS_WINDOW_H__

/*
===============================================================================

	The game window, without a single name of a graphics API.

	The platform layer (sys/win32/win_window.cpp) registers the window class, sets the
	display mode and creates the window. A render backend then attaches to the handle of
	Sys_GetNativeWindow and must release it again before Sys_DestroyWindow.

===============================================================================
*/

struct windowParms_t {
	int			x;				// ignored in fullscreen
	int			y;				// ignored in fullscreen
	int			width;
	int			height;
	int			fullScreen;		// 0 = windowed, otherwise 1 based monitor number to go full screen on
								// -1 = borderless window for spanning multiple displays
	bool		borderless;		// with fullScreen > 0: borderless window at the desktop resolution
								// of that monitor instead of exclusive fullscreen (width/height ignored)
	int			displayHz;
};

// Window class, display mode and window. False when the mode cannot be set; nothing is left behind then.
// Sets screenConfig.nativeScreenWidth, nativeScreenHeight, isFullscreen, displayFrequency and pixelAspect.
bool		Sys_CreateWindow( const windowParms_t & parms );

// Changes the mode or the size without destroying the window.
bool		Sys_SetWindowMode( const windowParms_t & parms );

// Closes the window and resets the display mode.
void		Sys_DestroyWindow();

// HWND on Windows, nullptr without a window.
void *		Sys_GetNativeWindow();

#endif /* !__SYS_WINDOW_H__ */
