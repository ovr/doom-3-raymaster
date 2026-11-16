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

#ifndef __GL_GLIMP_H__
#define __GL_GLIMP_H__

/*
===============================================================================

	The platform side of the OpenGL backend: pixel format, context and buffer swap
	on a window that sys_window.h created. Only OpenGL code includes this file;
	the Windows implementation is sys/win32/win_glimp.cpp.

===============================================================================
*/

struct glContextParms_t {
	void *		nativeWindow;	// from Sys_GetNativeWindow
	int			multiSamples;
	bool		stereo;
};

// what the platform found out while creating the context
struct glContextInfo_t {
	int				colorBits;
	int				depthBits;
	int				stencilBits;
	bool			stereoPixelFormatAvailable;	// a quad-buffer format exists, whether or not it was asked for
	bool			swapControlTearAvailable;
	const char *	wglExtensions;				// never nullptr
};

// Loads OpenGL, sets the pixel format of the window (found through the WGL bootstrap window) and
// creates the context. False leaves nothing behind and info undefined.
bool			GLimp_CreateContext( const glContextParms_t & parms, glContextInfo_t & info );

// Destroys the context and unloads OpenGL; the window stays.
void			GLimp_DestroyContext();

void			GLimp_SwapBuffers();

// 0 = no vsync, 1 = vsync, -1 = vsync that tears when a frame is late
void			GLimp_SetSwapInterval( int interval );

bool			GLimp_HasWGLExtension( const char * name );

// Returns a function pointer for an OpenGL extension entry point
GLExtension_t	GLimp_ExtensionPointer( const char * name );

#endif /* !__GL_GLIMP_H__ */
