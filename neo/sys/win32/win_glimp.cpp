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
/*
** WIN_GLIMP.C
**
** This file contains ALL Win32 specific stuff having to do with the
** OpenGL refresh: the pixel format, the WGL context and the buffer swap
** on the window that win_window.cpp created (renderer/OpenGL/gl_glimp.h).
**
** Note that the GLW_xxx functions are Windows specific GL-subsystem
** related functions that are relevant ONLY to win_glimp.c
*/
#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "win_local.h"
#include "rc/doom_resource.h"
#include "../../renderer/tr_local.h"
#include "../../renderer/OpenGL/wglext.h"		// windows OpenGL extensions
#include "../../renderer/OpenGL/gl_glimp.h"

// the WGL side of the window, private to this file
struct glwState_t {
	HWND			hWnd;			// the window of Sys_GetNativeWindow
	HDC				hDC;			// handle to device context
	HGLRC			hGLRC;			// handle to GL rendering context
	PIXELFORMATDESCRIPTOR pfd;
	int				pixelformat;

	bool			fakeClassRegistered;

	glContextInfo_t	info;			// handed to the OpenGL backend by GLimp_CreateContext

	// WGL_ARB_extensions_string
	PFNWGLGETEXTENSIONSSTRINGARBPROC		wglGetExtensionsStringARB;

	// WGL_EXT_swap_interval
	PFNWGLSWAPINTERVALEXTPROC				wglSwapIntervalEXT;

	// WGL_ARB_pixel_format
	PFNWGLGETPIXELFORMATATTRIBIVARBPROC		wglGetPixelFormatAttribivARB;
	PFNWGLGETPIXELFORMATATTRIBFVARBPROC		wglGetPixelFormatAttribfvARB;
	PFNWGLCHOOSEPIXELFORMATARBPROC			wglChoosePixelFormatARB;

	// WGL_ARB_create_context
	PFNWGLCREATECONTEXTATTRIBSARBPROC		wglCreateContextAttribsARB;
};

static glwState_t glw;


idCVar r_useOpenGL32( "r_useOpenGL32", "1", CVAR_INTEGER, "0 = OpenGL 2.0, 1 = OpenGL 3.2 compatibility profile, 2 = OpenGL 3.2 core profile", 0, 2 );

//
// function declaration
//
bool QGL_Init( const char *dllname );
void QGL_Shutdown();

/*
=============================================================================

WglExtension Grabbing

This is gross -- creating a window just to get a context to get the wgl extensions

=============================================================================
*/

/*
========================
GLimp_HasWGLExtension
========================
*/
bool GLimp_HasWGLExtension( const char * name ) {
	return glw.info.wglExtensions != nullptr && strstr( glw.info.wglExtensions, name ) != nullptr;
}

/*
========================
R_CheckWinExtension
========================
*/
static bool R_CheckWinExtension( const char * name ) {

	if ( !GLimp_HasWGLExtension( name ) ) {
		idLib::Printf( "X..%s not found\n", name );
		return false;
	}

	idLib::Printf( "...using %s\n", name );
	return true;
}


/*
====================
FakeWndProc

Only used to get wglExtensions
====================
*/
static LONG WINAPI FakeWndProc (
    HWND    hWnd,
    UINT    uMsg,
    WPARAM  wParam,
    LPARAM  lParam) {

	if ( uMsg != WM_CREATE ) {
	    return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}

	const static PIXELFORMATDESCRIPTOR pfd = {
		sizeof(PIXELFORMATDESCRIPTOR),
		1,
		PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
		PFD_TYPE_RGBA,
		24,
		0, 0, 0, 0, 0, 0,
		8, 0,
		0, 0, 0, 0,
		24, 8,
		0,
		PFD_MAIN_PLANE,
		0,
		0,
		0,
		0,
	};
	int		pixelFormat;
	HDC hDC;
	HGLRC hGLRC;

    hDC = GetDC(hWnd);

    // Set up OpenGL
    pixelFormat = ChoosePixelFormat(hDC, &pfd);
    SetPixelFormat(hDC, pixelFormat, &pfd);
    hGLRC = qwglCreateContext(hDC);
    qwglMakeCurrent(hDC, hGLRC);

	// free things
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hGLRC);
    ReleaseDC(hWnd, hDC);

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}


/*
==================
GLW_GetWGLExtensionsWithFakeWindow
==================
*/
static void GLW_CheckWGLExtensions( HDC hDC ) {
	glw.wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)
							  GLimp_ExtensionPointer("wglGetExtensionsStringARB");
	if ( glw.wglGetExtensionsStringARB ) {
		glw.info.wglExtensions = (const char *) glw.wglGetExtensionsStringARB(hDC);
	} else {
		glw.info.wglExtensions = "";
	}

	// WGL_EXT_swap_control
	glw.wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC) GLimp_ExtensionPointer( "wglSwapIntervalEXT" );
	r_swapInterval.SetModified();	// force a set next frame

	// WGL_EXT_swap_control_tear
	glw.info.swapControlTearAvailable = R_CheckWinExtension( "WGL_EXT_swap_control_tear" );

	// WGL_ARB_pixel_format
	glw.wglGetPixelFormatAttribivARB = (PFNWGLGETPIXELFORMATATTRIBIVARBPROC)GLimp_ExtensionPointer("wglGetPixelFormatAttribivARB");
	glw.wglGetPixelFormatAttribfvARB = (PFNWGLGETPIXELFORMATATTRIBFVARBPROC)GLimp_ExtensionPointer("wglGetPixelFormatAttribfvARB");
	glw.wglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)GLimp_ExtensionPointer("wglChoosePixelFormatARB");

	// wglCreateContextAttribsARB
	glw.wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress( "wglCreateContextAttribsARB" );
}

/*
====================
GLW_CreateFakeWindowClass

The class of the bootstrap window that is only used to get wgl extensions
====================
*/
static void GLW_CreateFakeWindowClass() {
	WNDCLASS wc;

	if ( glw.fakeClassRegistered ) {
		return;
	}

	memset( &wc, 0, sizeof( wc ) );

	wc.style         = 0;
	wc.lpfnWndProc   = (WNDPROC) FakeWndProc;
	wc.cbClsExtra    = 0;
	wc.cbWndExtra    = 0;
	wc.hInstance     = win32.hInstance;
	wc.hIcon         = LoadIcon( win32.hInstance, MAKEINTRESOURCE(IDI_ICON1));
	wc.hCursor       = LoadCursor (NULL,IDC_ARROW);
	wc.hbrBackground = (struct HBRUSH__ *)COLOR_GRAYTEXT;
	wc.lpszMenuName  = 0;
	wc.lpszClassName = WIN32_FAKE_WINDOW_CLASS_NAME;

	if ( !RegisterClass( &wc ) ) {
		common->FatalError( "GLW_CreateWindow: could not register window class" );
	}
	common->Printf( "...registered fake window class\n" );

	glw.fakeClassRegistered = true;
}

/*
==================
GLW_GetWGLExtensionsWithFakeWindow
==================
*/
static void GLW_GetWGLExtensionsWithFakeWindow() {
	HWND	hWnd;

    // Create a window for the sole purpose of getting
	// a valid context to get the wglextensions
    hWnd = CreateWindow(WIN32_FAKE_WINDOW_CLASS_NAME, GAME_NAME,
        WS_OVERLAPPEDWINDOW,
        40, 40,
        640,
        480,
        NULL, NULL, win32.hInstance, NULL );
    if ( !hWnd ) {
        common->FatalError( "GLW_GetWGLExtensionsWithFakeWindow: Couldn't create fake window" );
    }

    HDC hDC = GetDC( hWnd );
	HGLRC gRC = wglCreateContext( hDC );
	wglMakeCurrent( hDC, gRC );
	GLW_CheckWGLExtensions( hDC );
	wglDeleteContext( gRC );
	ReleaseDC( hWnd, hDC );

	// no message loop here: the game window exists by now and its messages
	// belong to the normal pump (FakeWndProc posts no quit message either)
    DestroyWindow( hWnd );
}

//=============================================================================

/*
========================
CreateOpenGLContextOnDC
========================
*/
static HGLRC CreateOpenGLContextOnDC( const HDC hdc, const bool debugContext ) {
	int useOpenGL32 = r_useOpenGL32.GetInteger();
	HGLRC m_hrc = NULL;

	for ( int i = 0; i < 2; i++ ) {
		const int glMajorVersion = ( useOpenGL32 != 0 ) ? 3 : 2;
		const int glMinorVersion = ( useOpenGL32 != 0 ) ? 2 : 0;
		const int glDebugFlag = debugContext ? WGL_CONTEXT_DEBUG_BIT_ARB : 0;
		const int glProfileMask = ( useOpenGL32 != 0 ) ? WGL_CONTEXT_PROFILE_MASK_ARB : 0;
		const int glProfile = ( useOpenGL32 == 1 ) ? WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB : ( ( useOpenGL32 == 2 ) ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB : 0 );
		const int attribs[] =
		{
			WGL_CONTEXT_MAJOR_VERSION_ARB,	glMajorVersion,
			WGL_CONTEXT_MINOR_VERSION_ARB,	glMinorVersion,
			WGL_CONTEXT_FLAGS_ARB,			glDebugFlag,
			glProfileMask,					glProfile,
			0
		};

		m_hrc = glw.wglCreateContextAttribsARB( hdc, 0, attribs );
		if ( m_hrc != NULL ) {
			idLib::Printf( "created OpenGL %d.%d context\n", glMajorVersion, glMinorVersion );
			break;
		}

		idLib::Printf( "failed to create OpenGL %d.%d context\n", glMajorVersion, glMinorVersion );
		useOpenGL32 = 0;	// fall back to OpenGL 2.0
	}

	if ( m_hrc == NULL ) {
		int	err = GetLastError();
		switch( err ) {
			case ERROR_INVALID_VERSION_ARB: idLib::Printf( "ERROR_INVALID_VERSION_ARB\n" ); break;
			case ERROR_INVALID_PROFILE_ARB: idLib::Printf( "ERROR_INVALID_PROFILE_ARB\n" ); break;
			default: idLib::Printf( "unknown error: 0x%x\n", err ); break;
		}
	}

	return m_hrc;
}

/*
====================
GLW_ChoosePixelFormat

Returns -1 on failure, or a pixel format
====================
*/
static int GLW_ChoosePixelFormat( const HDC hdc, const int multisamples, const bool stereo3D ) {
	FLOAT	fAttributes[] = { 0, 0 };
	int		iAttributes[] = {
		WGL_SAMPLE_BUFFERS_ARB, ( ( multisamples > 1 ) ? 1 : 0 ),
		WGL_SAMPLES_ARB, multisamples,
		WGL_DOUBLE_BUFFER_ARB, TRUE,
		WGL_STENCIL_BITS_ARB, 8,
		WGL_DEPTH_BITS_ARB, 24,
		WGL_RED_BITS_ARB, 8,
		WGL_BLUE_BITS_ARB, 8,
		WGL_GREEN_BITS_ARB, 8,
		WGL_ALPHA_BITS_ARB, 8,
		WGL_STEREO_ARB, ( stereo3D ? TRUE : FALSE ),
		0, 0
	};

	int	pixelFormat;
	UINT numFormats;
	if ( !glw.wglChoosePixelFormatARB( hdc, iAttributes, fAttributes, 1, &pixelFormat, &numFormats ) ) {
		return -1;
	}
	return pixelFormat;
}


/*
====================
GLW_InitDriver

Set the pixelformat for the window before it is
shown, and create the rendering context
====================
*/
static bool GLW_InitDriver( const glContextParms_t & parms ) {
    PIXELFORMATDESCRIPTOR src = 
	{
		sizeof(PIXELFORMATDESCRIPTOR),	// size of this pfd
		1,								// version number
		PFD_DRAW_TO_WINDOW |			// support window
		PFD_SUPPORT_OPENGL |			// support OpenGL
		PFD_DOUBLEBUFFER,				// double buffered
		PFD_TYPE_RGBA,					// RGBA type
		32,								// 32-bit color depth
		0, 0, 0, 0, 0, 0,				// color bits ignored
		8,								// 8 bit destination alpha
		0,								// shift bit ignored
		0,								// no accumulation buffer
		0, 0, 0, 0, 					// accum bits ignored
		24,								// 24-bit z-buffer	
		8,								// 8-bit stencil buffer
		0,								// no auxiliary buffer
		PFD_MAIN_PLANE,					// main layer
		0,								// reserved
		0, 0, 0							// layer masks ignored
    };

	common->Printf( "Initializing OpenGL driver\n" );

	//
	// get a DC for our window if we don't already have one allocated
	//
	if ( glw.hDC == NULL ) {
		common->Printf( "...getting DC: " );

		if ( ( glw.hDC = GetDC( glw.hWnd ) ) == NULL ) {
			common->Printf( "^3failed^0\n" );
			return false;
		}
		common->Printf( "succeeded\n" );
	}

	// the multisample path uses the wgl 
	if ( glw.wglChoosePixelFormatARB ) {
		glw.pixelformat = GLW_ChoosePixelFormat( glw.hDC, parms.multiSamples, parms.stereo );
	} else {
		// this is the "classic" choose pixel format path
		common->Printf( "Using classic ChoosePixelFormat\n" );

		// eventually we may need to have more fallbacks, but for
		// now, ask for everything
		if ( parms.stereo ) {
			common->Printf( "...attempting to use stereo\n" );
			src.dwFlags |= PFD_STEREO;
		}

		//
		// choose, set, and describe our desired pixel format.  If we're
		// using a minidriver then we need to bypass the GDI functions,
		// otherwise use the GDI functions.
		//
		if ( ( glw.pixelformat = ChoosePixelFormat( glw.hDC, &src ) ) == 0 ) {
			common->Printf( "...^3GLW_ChoosePFD failed^0\n");
			return false;
		}
		common->Printf( "...PIXELFORMAT %d selected\n", glw.pixelformat );
	}

	// get the full info
	DescribePixelFormat( glw.hDC, glw.pixelformat, sizeof( glw.pfd ), &glw.pfd );
	glw.info.colorBits = glw.pfd.cColorBits;
	glw.info.depthBits = glw.pfd.cDepthBits;
	glw.info.stencilBits = glw.pfd.cStencilBits;

	// XP seems to set this incorrectly
	if ( !glw.info.stencilBits ) {
		glw.info.stencilBits = 8;
	}

	// the same SetPixelFormat is used either way
	if ( SetPixelFormat( glw.hDC, glw.pixelformat, &glw.pfd ) == FALSE ) {
		common->Printf( "...^3SetPixelFormat failed^0\n", glw.hDC );
		return false;
	}

	//
	// startup the OpenGL subsystem by creating a context and making it current
	//
	common->Printf( "...creating GL context: " );
	glw.hGLRC = CreateOpenGLContextOnDC( glw.hDC, r_debugContext.GetBool() );
	if ( glw.hGLRC == 0 ) {
		common->Printf( "^3failed^0\n" );
		return false;
	}
	common->Printf( "succeeded\n" );

	common->Printf( "...making context current: " );
	if ( !qwglMakeCurrent( glw.hDC, glw.hGLRC ) ) {
		qwglDeleteContext( glw.hGLRC );
		glw.hGLRC = NULL;
		common->Printf( "^3failed^0\n" );
		return false;
	}
	common->Printf( "succeeded\n" );

	return true;
}

/*
===================
GLimp_CreateContext

Loads OpenGL, gets the WGL extensions through the bootstrap window, then
sets the pixel format of the game window and creates the context on it.

If there is any failure, the renderer will revert back to safe
parameters and try again.
===================
*/
bool GLimp_CreateContext( const glContextParms_t & parms, glContextInfo_t & info ) {
	common->Printf( "Initializing OpenGL subsystem with multisamples:%i stereo:%i\n", parms.multiSamples, parms.stereo );

	glw.hWnd = static_cast<HWND>( parms.nativeWindow );

	GLW_CreateFakeWindowClass();

	// this will load the dll and set all our qgl* function pointers,
	// but doesn't create a window
	if ( !QGL_Init( "opengl32" ) ) {
		common->Printf( "^3GLimp_CreateContext() could not load opengl32^0\n" );
		return false;
	}

	// getting the wgl extensions involves creating a fake window to get a context,
	// which is pretty disgusting, and seems to mess with the AGP VAR allocation
	GLW_GetWGLExtensionsWithFakeWindow();

	// makeCurrent NULL frees the DC, so get another
	glw.hDC = GetDC( glw.hWnd );
	if ( !glw.hDC ) {
		common->Printf( "^3GLimp_CreateContext() - GetDC()failed^0\n" );
		GLimp_DestroyContext();
		return false;
	}

	// Check to see if we can get a stereo pixel format, even if we aren't going to use it,
	// so the menu option can be
	if ( GLW_ChoosePixelFormat( glw.hDC, parms.multiSamples, true ) != -1 ) {
		glw.info.stereoPixelFormatAvailable = true;
	} else {
		glw.info.stereoPixelFormatAvailable = false;
	}

	if ( !GLW_InitDriver( parms ) ) {
		GLimp_DestroyContext();
		return false;
	}

	// wglSwapinterval, etc
	GLW_CheckWGLExtensions( glw.hDC );

	info = glw.info;
	return true;
}

/*
===================
GLimp_DestroyContext

Destroys the rendering context and unloads OpenGL. The window stays:
Sys_DestroyWindow closes it afterwards.
===================
*/
void GLimp_DestroyContext() {
	const char *success[] = { "failed", "success" };
	int retVal;

	common->Printf( "Shutting down OpenGL subsystem\n" );

	// set current context to NULL
	if ( qwglMakeCurrent ) {
		retVal = qwglMakeCurrent( NULL, NULL ) != 0;
		common->Printf( "...wglMakeCurrent( NULL, NULL ): %s\n", success[retVal] );
	}

	// delete HGLRC
	if ( glw.hGLRC ) {
		retVal = qwglDeleteContext( glw.hGLRC ) != 0;
		common->Printf( "...deleting GL context: %s\n", success[retVal] );
		glw.hGLRC = NULL;
	}

	// release DC
	if ( glw.hDC ) {
		retVal = ReleaseDC( glw.hWnd, glw.hDC ) != 0;
		common->Printf( "...releasing DC: %s\n", success[retVal] );
		glw.hDC   = NULL;
	}
	glw.hWnd = NULL;

	// shutdown QGL subsystem
	QGL_Shutdown();
}

/*
=====================
GLimp_SetSwapInterval
=====================
*/
void GLimp_SetSwapInterval( int interval ) {
	if ( glw.wglSwapIntervalEXT ) {
		glw.wglSwapIntervalEXT( interval );
	}
}

/*
=====================
GLimp_SwapBuffers
=====================
*/
void GLimp_SwapBuffers() {
	if ( r_swapInterval.IsModified() ) {
		r_swapInterval.ClearModified();

		int interval = 0;
		if ( r_swapInterval.GetInteger() == 1 ) {
			interval = ( glw.info.swapControlTearAvailable ) ? -1 : 1;
		} else if ( r_swapInterval.GetInteger() == 2 ) {
			interval = 1;
		}

		GLimp_SetSwapInterval( interval );
	}

	qwglSwapBuffers( glw.hDC );
}

/*
===================
GLimp_ExtensionPointer

Returns a function pointer for an OpenGL extension entry point
===================
*/
GLExtension_t GLimp_ExtensionPointer( const char *name ) {
	void	(*proc)();

	proc = (GLExtension_t)qwglGetProcAddress( name );

	if ( !proc ) {
		common->Printf( "Couldn't find proc address for: %s\n", name );
	}

	return proc;
}
