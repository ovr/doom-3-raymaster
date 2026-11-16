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

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "gl_Backend.h"
#include "gl_glimp.h"

static idRenderBackendFactory glFactory( "opengl", []() -> idRenderBackend * { return new idRenderBackendGL; } );

/*
========================
NotMigrated

The body of this method still lives in the old OpenGL path and nothing may call it yet.
========================
*/
static void NotMigrated( const char * method ) {
	common->FatalError( "idRenderBackendGL::%s is not migrated yet", method );
}

/*
========================
idRenderBackendGL::idRenderBackendGL
========================
*/
idRenderBackendGL::idRenderBackendGL() {
	ownerThread = Sys_GetCurrentThreadID();
}

// the extension list of a context without GL_EXTENSIONS, built from glGetStringi
static idStr extensions_string;

static PFNGLGETSTRINGIPROC qglGetStringi;

/*
========================
idRenderBackendGL::Init

Creates the context on the window and finds out what it can do. False = no context with these
parms, nothing is left behind and the caller may try other parms.
========================
*/
bool idRenderBackendGL::Init( const renderBackendInit_t & init ) {
	glContextParms_t contextParms;
	contextParms.nativeWindow = init.nativeWindow;
	contextParms.multiSamples = init.multiSamples;
	contextParms.stereo = init.stereoPixelFormat;

	glContextInfo_t contextInfo = {};
	if ( !GLimp_CreateContext( contextParms, contextInfo ) ) {
		return false;
	}

	glContext = {};
	glContext.colorBits = contextInfo.colorBits;
	glContext.depthBits = contextInfo.depthBits;
	glContext.stencilBits = contextInfo.stencilBits;
	glContext.stereoPixelFormatAvailable = contextInfo.stereoPixelFormatAvailable;
	glContext.isStereoPixelFormat = init.stereoPixelFormat;
	glContext.multisamples = init.multiSamples;
	glContext.wgl_extensions_string = contextInfo.wglExtensions;
	glContext.swapControlTearAvailable = contextInfo.swapControlTearAvailable;

	// get our config strings
	glContext.vendor_string = (const char *)qglGetString( GL_VENDOR );
	glContext.renderer_string = (const char *)qglGetString( GL_RENDERER );
	glContext.version_string = (const char *)qglGetString( GL_VERSION );
	glContext.shading_language_string = (const char *)qglGetString( GL_SHADING_LANGUAGE_VERSION );
	glContext.extensions_string = (const char *)qglGetString( GL_EXTENSIONS );

	if ( glContext.extensions_string == NULL ) {
		// As of OpenGL 3.2, glGetStringi is required to obtain the available extensions
		qglGetStringi = (PFNGLGETSTRINGIPROC)GLimp_ExtensionPointer( "glGetStringi" );

		// Build the extensions string
		GLint numExtensions;
		qglGetIntegerv( GL_NUM_EXTENSIONS, &numExtensions );
		extensions_string.Clear();
		for ( int i = 0; i < numExtensions; i++ ) {
			extensions_string.Append( (const char*)qglGetStringi( GL_EXTENSIONS, i ) );
			// the now deprecated glGetString method usaed to create a single string with each extension separated by a space
			if ( i < numExtensions - 1 ) {
				extensions_string.Append( ' ' );
			}
		}
		glContext.extensions_string = extensions_string.c_str();
	}


	float glVersion = atof( glContext.version_string );
	float glslVersion = atof( glContext.shading_language_string );
	idLib::Printf( "OpenGL Version: %3.1f\n", glVersion );
	idLib::Printf( "OpenGL Vendor : %s\n", glContext.vendor_string );
	idLib::Printf( "OpenGL GLSL   : %3.1f\n", glslVersion );
	
	// OpenGL driver constants
	GLint temp;
	qglGetIntegerv( GL_MAX_TEXTURE_SIZE, &temp );
	glContext.maxTextureSize = temp;

	// stubbed or broken drivers may have reported 0...
	if ( glContext.maxTextureSize <= 0 ) {
		glContext.maxTextureSize = 256;
	}

	// recheck all the extensions (FIXME: this might be dangerous)
	R_CheckPortableExtensions();

	// maxSamples and maxDebugGroupDepth stay 0: asking GL for them would add calls to the init sequence
	caps = {};
	caps.apiName = "OpenGL";
	caps.vendorName = glContext.vendor_string;
	caps.rendererName = glContext.renderer_string;
	caps.versionString = glContext.version_string;
	caps.maxTextureSize = glContext.maxTextureSize;
	caps.maxAnisotropy = glContext.maxTextureAnisotropy;
	caps.uniformBufferOffsetAlignment = glContext.uniformBufferOffsetAlignment;
	caps.depthBits = glContext.depthBits;
	caps.depthBoundsTest = glContext.depthBoundsTestAvailable;
	caps.maskedColorClear = true;
	caps.quadBufferStereo = glContext.stereoPixelFormatAvailable;
	caps.adaptiveSync = glContext.swapControlTearAvailable;
	caps.gpuTimer = glContext.timerQueryAvailable;
	caps.timestamps = glContext.timestampQueryAvailable;
	caps.debugGroups = glContext.debugGroupsAvailable;

	outputWidth = init.width;
	outputHeight = init.height;
	return true;
}

/*
========================
idRenderBackendGL::Shutdown

Safe without a context: Sys_Error calls it in whatever state the renderer is.
========================
*/
void idRenderBackendGL::Shutdown() {
	GLimp_DestroyContext();
}

/*
========================
idRenderBackendGL::ResizeOutput
========================
*/
void idRenderBackendGL::ResizeOutput( int width, int height ) {
	outputWidth = width;
	outputHeight = height;
}

/*
========================
idRenderBackendGL::SetOption
========================
*/
void idRenderBackendGL::SetOption( renderOption_t option, bool enable ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::SetGammaRamp
========================
*/
void idRenderBackendGL::SetGammaRamp( const unsigned short red[256], const unsigned short green[256], const unsigned short blue[256] ) {
	Sys_SetDeviceGammaRamp( red, green, blue );
}

/*
========================
idRenderBackendGL::CheckErrors
========================
*/
void idRenderBackendGL::CheckErrors() {
    int		err;
    char	s[64];
	int		i;

	// check for up to 10 errors pending
	for ( i = 0 ; i < 10 ; i++ ) {
		err = qglGetError();
		if ( err == GL_NO_ERROR ) {
			return;
		}
		switch( err ) {
			case GL_INVALID_ENUM:
				strcpy( s, "GL_INVALID_ENUM" );
				break;
			case GL_INVALID_VALUE:
				strcpy( s, "GL_INVALID_VALUE" );
				break;
			case GL_INVALID_OPERATION:
				strcpy( s, "GL_INVALID_OPERATION" );
				break;
			case GL_STACK_OVERFLOW:
				strcpy( s, "GL_STACK_OVERFLOW" );
				break;
			case GL_STACK_UNDERFLOW:
				strcpy( s, "GL_STACK_UNDERFLOW" );
				break;
			case GL_OUT_OF_MEMORY:
				strcpy( s, "GL_OUT_OF_MEMORY" );
				break;
			default:
				idStr::snPrintf( s, sizeof(s), "%i", err);
				break;
		}

		if ( !r_ignoreGLErrors.GetBool() ) {
			common->Printf( "GL_CheckErrors: %s\n", s );
		}
	}
}

/*
========================
idRenderBackendGL::PrintInfo
========================
*/
void idRenderBackendGL::PrintInfo() const {
	const char *fsstrings[] =
	{
		"windowed",
		"fullscreen"
	};

	common->Printf( "\nGL_VENDOR: %s\n", glContext.vendor_string );
	common->Printf( "GL_RENDERER: %s\n", glContext.renderer_string );
	common->Printf( "GL_VERSION: %s\n", glContext.version_string );
	common->Printf( "GL_EXTENSIONS: %s\n", glContext.extensions_string );
	if ( glContext.wgl_extensions_string ) {
		common->Printf( "WGL_EXTENSIONS: %s\n", glContext.wgl_extensions_string );
	}
	common->Printf( "GL_MAX_TEXTURE_SIZE: %d\n", glContext.maxTextureSize );
	common->Printf( "GL_MAX_TEXTURE_COORDS_ARB: %d\n", glContext.maxTextureCoords );
	common->Printf( "GL_MAX_TEXTURE_IMAGE_UNITS_ARB: %d\n", glContext.maxTextureImageUnits );

	// print all the display adapters, monitors, and video modes
	void DumpAllDisplayDevices();
	DumpAllDisplayDevices();

	common->Printf( "\nPIXELFORMAT: color(%d-bits) Z(%d-bit) stencil(%d-bits)\n", glContext.colorBits, glContext.depthBits, glContext.stencilBits );
	common->Printf( "MODE: %d, %d x %d %s hz:", r_vidMode.GetInteger(), renderSystem->GetWidth(), renderSystem->GetHeight(), fsstrings[r_fullscreen.GetBool()] );
	if ( glConfig.displayFrequency ) {
		common->Printf( "%d\n", glConfig.displayFrequency );
	} else {
		common->Printf( "N/A\n" );
	}

	common->Printf( "-------\n" );

	if ( r_swapInterval.GetInteger() && GLimp_HasWGLExtension( "WGL_EXT_swap_control" ) ) {
		common->Printf( "Forcing swapInterval %i\n", r_swapInterval.GetInteger() );
	} else {
		common->Printf( "swapInterval not forced\n" );
	}

	if ( glContext.stereoPixelFormatAvailable && glContext.isStereoPixelFormat ) {
		idLib::Printf( "OpenGl quad buffer stereo pixel format active\n" );
	} else if ( glContext.stereoPixelFormatAvailable ) {
		idLib::Printf( "OpenGl quad buffer stereo pixel available but not selected\n" );
	} else {
		idLib::Printf( "OpenGl quad buffer stereo pixel format not available\n" );
	}

	idLib::Printf( "Stereo mode: " );
	switch ( renderSystem->GetStereo3DMode() ) {
		case STEREO3D_OFF:						idLib::Printf( "STEREO3D_OFF\n" ); break;
		case STEREO3D_SIDE_BY_SIDE_COMPRESSED:	idLib::Printf( "STEREO3D_SIDE_BY_SIDE_COMPRESSED\n" ); break;
		case STEREO3D_TOP_AND_BOTTOM_COMPRESSED:idLib::Printf( "STEREO3D_TOP_AND_BOTTOM_COMPRESSED\n" ); break;
		case STEREO3D_SIDE_BY_SIDE:				idLib::Printf( "STEREO3D_SIDE_BY_SIDE\n" ); break;
		case STEREO3D_HDMI_720:					idLib::Printf( "STEREO3D_HDMI_720\n" ); break;
		case STEREO3D_INTERLACED:				idLib::Printf( "STEREO3D_INTERLACED\n" ); break;
		case STEREO3D_QUAD_BUFFER:				idLib::Printf( "STEREO3D_QUAD_BUFFER\n" ); break;
		default:idLib::Printf( "Unknown (%i)\n", renderSystem->GetStereo3DMode() ); break;
	}

	idLib::Printf( "%i multisamples\n", glContext.multisamples );

	common->Printf( "%5.1f cm screen width (%4.1f\" diagonal)\n",
		glConfig.physicalScreenWidthInCentimeters, glConfig.physicalScreenWidthInCentimeters / 2.54f
			* sqrt( (float)(16*16 + 9*9) ) / 16.0f );
	extern idCVar r_forceScreenWidthCentimeters;
	if ( r_forceScreenWidthCentimeters.GetFloat() ) {
		common->Printf( "screen size manually forced to %5.1f cm width (%4.1f\" diagonal)\n",
			renderSystem->GetPhysicalScreenWidthInCentimeters(), renderSystem->GetPhysicalScreenWidthInCentimeters() / 2.54f
				* sqrt( (float)(16*16 + 9*9) ) / 16.0f );
	}
}

/*
========================
idRenderBackendGL::BeginFrame
========================
*/
void idRenderBackendGL::BeginFrame() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::EndFrame
========================
*/
void idRenderBackendGL::EndFrame() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::Present
========================
*/
presentResult_t idRenderBackendGL::Present( presentMode_t mode ) {
	NotMigrated( __func__ );
	return presentResult_t::OK;
}

/*
========================
idRenderBackendGL::GetFrameGpuMicroseconds
========================
*/
uint64 idRenderBackendGL::GetFrameGpuMicroseconds() {
	NotMigrated( __func__ );
	return 0;
}

/*
========================
idRenderBackendGL::SelectOutput
========================
*/
void idRenderBackendGL::SelectOutput( outputEye_t eye ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::ReadOutputPixels
========================
*/
void idRenderBackendGL::ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::WriteTimestamp
========================
*/
void idRenderBackendGL::WriteTimestamp( int set, int index ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::TimestampReady
========================
*/
bool idRenderBackendGL::TimestampReady( int set, int index ) {
	NotMigrated( __func__ );
	return false;
}

/*
========================
idRenderBackendGL::ReadTimestamps
========================
*/
void idRenderBackendGL::ReadTimestamps( int set, int count, uint64 * nanoseconds ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::FreeTimestamps
========================
*/
void idRenderBackendGL::FreeTimestamps() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::PushDebugGroup
========================
*/
void idRenderBackendGL::PushDebugGroup( const char * name ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::PopDebugGroup
========================
*/
void idRenderBackendGL::PopDebugGroup() {
	NotMigrated( __func__ );
}
