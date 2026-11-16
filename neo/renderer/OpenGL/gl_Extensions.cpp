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

extern idCVar r_useSeamlessCubeMap;
extern idCVar r_useSRGB;

// what the context can do; filled by idRenderBackendGL::Init
glContext_t	glContext;

// GL_ARB_multitexture
PFNGLACTIVETEXTUREPROC					qglActiveTextureARB;
PFNGLCLIENTACTIVETEXTUREPROC			qglClientActiveTextureARB;

// GL_EXT_direct_state_access
PFNGLBINDMULTITEXTUREEXTPROC			qglBindMultiTextureEXT;

// GL_ARB_texture_compression
PFNGLCOMPRESSEDTEXIMAGE2DARBPROC		qglCompressedTexImage2DARB;
PFNGLCOMPRESSEDTEXSUBIMAGE2DARBPROC		qglCompressedTexSubImage2DARB;
PFNGLGETCOMPRESSEDTEXIMAGEARBPROC		qglGetCompressedTexImageARB;

// GL_ARB_vertex_buffer_object
PFNGLBINDBUFFERARBPROC					qglBindBufferARB;
PFNGLBINDBUFFERRANGEPROC				qglBindBufferRange;
PFNGLDELETEBUFFERSARBPROC				qglDeleteBuffersARB;
PFNGLGENBUFFERSARBPROC					qglGenBuffersARB;
PFNGLISBUFFERARBPROC					qglIsBufferARB;
PFNGLBUFFERDATAARBPROC					qglBufferDataARB;
PFNGLBUFFERSUBDATAARBPROC				qglBufferSubDataARB;
PFNGLGETBUFFERSUBDATAARBPROC			qglGetBufferSubDataARB;
PFNGLMAPBUFFERARBPROC					qglMapBufferARB;
PFNGLUNMAPBUFFERARBPROC					qglUnmapBufferARB;
PFNGLGETBUFFERPARAMETERIVARBPROC		qglGetBufferParameterivARB;
PFNGLGETBUFFERPOINTERVARBPROC			qglGetBufferPointervARB;

// GL_ARB_map_buffer_range
PFNGLMAPBUFFERRANGEPROC					qglMapBufferRange;

// GL_ARB_draw_elements_base_vertex
PFNGLDRAWELEMENTSBASEVERTEXPROC  		qglDrawElementsBaseVertex;

// GL_ARB_vertex_array_object
PFNGLGENVERTEXARRAYSPROC				qglGenVertexArrays;
PFNGLBINDVERTEXARRAYPROC				qglBindVertexArray;
PFNGLDELETEVERTEXARRAYSPROC				qglDeleteVertexArrays;

// GL_ARB_vertex_program / GL_ARB_fragment_program
PFNGLVERTEXATTRIBPOINTERARBPROC			qglVertexAttribPointerARB;
PFNGLENABLEVERTEXATTRIBARRAYARBPROC		qglEnableVertexAttribArrayARB;
PFNGLDISABLEVERTEXATTRIBARRAYARBPROC	qglDisableVertexAttribArrayARB;

// GLSL / OpenGL 2.0
PFNGLCREATESHADERPROC					qglCreateShader;
PFNGLDELETESHADERPROC					qglDeleteShader;
PFNGLSHADERSOURCEPROC					qglShaderSource;
PFNGLCOMPILESHADERPROC					qglCompileShader;
PFNGLGETSHADERIVPROC					qglGetShaderiv;
PFNGLGETSHADERINFOLOGPROC				qglGetShaderInfoLog;
PFNGLCREATEPROGRAMPROC					qglCreateProgram;
PFNGLDELETEPROGRAMPROC					qglDeleteProgram;
PFNGLATTACHSHADERPROC					qglAttachShader;
PFNGLLINKPROGRAMPROC					qglLinkProgram;
PFNGLUSEPROGRAMPROC						qglUseProgram;
PFNGLGETPROGRAMIVPROC					qglGetProgramiv;
PFNGLGETPROGRAMINFOLOGPROC				qglGetProgramInfoLog;
PFNGLBINDATTRIBLOCATIONPROC				qglBindAttribLocation;
PFNGLGETUNIFORMLOCATIONPROC				qglGetUniformLocation;
PFNGLUNIFORM1IPROC						qglUniform1i;
PFNGLUNIFORM4FVPROC						qglUniform4fv;

// GL_ARB_uniform_buffer_object
PFNGLGETUNIFORMBLOCKINDEXPROC			qglGetUniformBlockIndex;
PFNGLUNIFORMBLOCKBINDINGPROC			qglUniformBlockBinding;

// GL_ATI_separate_stencil / OpenGL 2.0
PFNGLSTENCILOPSEPARATEATIPROC			qglStencilOpSeparate;
PFNGLSTENCILFUNCSEPARATEATIPROC			qglStencilFuncSeparate;

// GL_EXT_depth_bounds_test
PFNGLDEPTHBOUNDSEXTPROC                 qglDepthBoundsEXT;

// GL_ARB_sync
PFNGLFENCESYNCPROC						qglFenceSync;
PFNGLISSYNCPROC							qglIsSync;
PFNGLCLIENTWAITSYNCPROC					qglClientWaitSync;
PFNGLDELETESYNCPROC						qglDeleteSync;

// GL_ARB_occlusion_query
PFNGLGENQUERIESARBPROC					qglGenQueriesARB;
PFNGLDELETEQUERIESARBPROC				qglDeleteQueriesARB;
PFNGLISQUERYARBPROC						qglIsQueryARB;
PFNGLBEGINQUERYARBPROC					qglBeginQueryARB;
PFNGLENDQUERYARBPROC					qglEndQueryARB;
PFNGLGETQUERYIVARBPROC					qglGetQueryivARB;
PFNGLGETQUERYOBJECTIVARBPROC			qglGetQueryObjectivARB;
PFNGLGETQUERYOBJECTUIVARBPROC			qglGetQueryObjectuivARB;

// GL_ARB_timer_query / GL_EXT_timer_query
PFNGLGETQUERYOBJECTUI64VEXTPROC			qglGetQueryObjectui64vEXT;

// GL_ARB_timer_query
PFNGLQUERYCOUNTERPROC					qglQueryCounter;

// GL_KHR_debug
PFNGLPUSHDEBUGGROUPPROC					qglPushDebugGroup;
PFNGLPOPDEBUGGROUPPROC					qglPopDebugGroup;

// GL_ARB_debug_output
PFNGLDEBUGMESSAGECONTROLARBPROC			qglDebugMessageControlARB;
PFNGLDEBUGMESSAGEINSERTARBPROC			qglDebugMessageInsertARB;
PFNGLDEBUGMESSAGECALLBACKARBPROC		qglDebugMessageCallbackARB;
PFNGLGETDEBUGMESSAGELOGARBPROC			qglGetDebugMessageLogARB;

/*
========================
glBindMultiTextureEXT

As of 2011/09/16 the Intel drivers for "Sandy Bridge" and "Ivy Bridge" integrated graphics do not support this extension.
========================
*/
void APIENTRY glBindMultiTextureEXT( GLenum texunit, GLenum target, GLuint texture ) {
	qglActiveTextureARB( texunit );
	qglBindTexture( target, texture );
}


/*
=================
R_CheckExtension
=================
*/
bool R_CheckExtension( const char *name ) {
	if ( !strstr( glContext.extensions_string, name ) ) {
		common->Printf( "X..%s not found\n", name );
		return false;
	}

	common->Printf( "...using %s\n", name );
	return true;
}


/*
========================
DebugCallback

For ARB_debug_output
========================
*/
static void CALLBACK DebugCallback(unsigned int source, unsigned int type,
								   unsigned int id, unsigned int severity, int length, const char * message, void * userParam) {
	// every r_pix 2 debug group would echo here
	if ( type == GL_DEBUG_TYPE_PUSH_GROUP || type == GL_DEBUG_TYPE_POP_GROUP ) {
		return;
	}
	// it probably isn't safe to do an idLib::Printf at this point
	OutputDebugString( message );
	OutputDebugString( "\n" );
}

/*
==================
R_CheckPortableExtensions
==================
*/
void R_CheckPortableExtensions() {
	glContext.glVersion = atof( glContext.version_string );
	const char * badVideoCard = idLocalization::GetString( "#str_06780" );
	if ( glContext.glVersion < 2.0f ) {
		idLib::FatalError( badVideoCard );
	}

	if ( idStr::Icmpn( glContext.renderer_string, "ATI ", 4 ) == 0 || idStr::Icmpn( glContext.renderer_string, "AMD ", 4 ) == 0 ) {
		glContext.vendor = VENDOR_AMD;
	} else if ( idStr::Icmpn( glContext.renderer_string, "NVIDIA", 6 ) == 0 ) {
		glContext.vendor = VENDOR_NVIDIA;
	} else if ( idStr::Icmpn( glContext.renderer_string, "Intel", 5 ) == 0 ) {
		glContext.vendor = VENDOR_INTEL;
	}

	// GL_ARB_multitexture
	glContext.multitextureAvailable = R_CheckExtension( "GL_ARB_multitexture" );
	if ( glContext.multitextureAvailable ) {
		qglActiveTextureARB = (void(APIENTRY *)(GLenum))GLimp_ExtensionPointer( "glActiveTextureARB" );
		qglClientActiveTextureARB = (void(APIENTRY *)(GLenum))GLimp_ExtensionPointer( "glClientActiveTextureARB" );
	}

	// GL_EXT_direct_state_access
	glContext.directStateAccess = R_CheckExtension( "GL_EXT_direct_state_access" );
	if ( glContext.directStateAccess ) {
		qglBindMultiTextureEXT = (PFNGLBINDMULTITEXTUREEXTPROC)GLimp_ExtensionPointer( "glBindMultiTextureEXT" );
	} else {
		qglBindMultiTextureEXT = glBindMultiTextureEXT;
	}

	// GL_ARB_texture_compression + GL_S3_s3tc
	// DRI drivers may have GL_ARB_texture_compression but no GL_EXT_texture_compression_s3tc
	glContext.textureCompressionAvailable = R_CheckExtension( "GL_ARB_texture_compression" ) && R_CheckExtension( "GL_EXT_texture_compression_s3tc" );
	if ( glContext.textureCompressionAvailable ) {
		qglCompressedTexImage2DARB = (PFNGLCOMPRESSEDTEXIMAGE2DARBPROC)GLimp_ExtensionPointer( "glCompressedTexImage2DARB" );
		qglCompressedTexSubImage2DARB = (PFNGLCOMPRESSEDTEXSUBIMAGE2DARBPROC)GLimp_ExtensionPointer( "glCompressedTexSubImage2DARB" );
		qglGetCompressedTexImageARB = (PFNGLGETCOMPRESSEDTEXIMAGEARBPROC)GLimp_ExtensionPointer( "glGetCompressedTexImageARB" );
	}

	// GL_EXT_texture_filter_anisotropic
	glContext.anisotropicFilterAvailable = R_CheckExtension( "GL_EXT_texture_filter_anisotropic" );
	if ( glContext.anisotropicFilterAvailable ) {
		qglGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &glContext.maxTextureAnisotropy );
		common->Printf( "   maxTextureAnisotropy: %f\n", glContext.maxTextureAnisotropy );
	} else {
		glContext.maxTextureAnisotropy = 1;
	}

	// GL_EXT_texture_lod_bias
	// The actual extension is broken as specificed, storing the state in the texture unit instead
	// of the texture object.  The behavior in GL 1.4 is the behavior we use.
	glContext.textureLODBiasAvailable = ( glContext.glVersion >= 1.4 || R_CheckExtension( "GL_EXT_texture_lod_bias" ) );
	if ( glContext.textureLODBiasAvailable ) {
		common->Printf( "...using %s\n", "GL_EXT_texture_lod_bias" );
	} else {
		common->Printf( "X..%s not found\n", "GL_EXT_texture_lod_bias" );
	}

	// GL_ARB_seamless_cube_map
	glContext.seamlessCubeMapAvailable = R_CheckExtension( "GL_ARB_seamless_cube_map" );
	r_useSeamlessCubeMap.SetModified();		// the CheckCvars() next frame will enable / disable it

	// GL_ARB_framebuffer_sRGB
	glContext.sRGBFramebufferAvailable = R_CheckExtension( "GL_ARB_framebuffer_sRGB" );
	r_useSRGB.SetModified();		// the CheckCvars() next frame will enable / disable it

	// GL_ARB_vertex_buffer_object
	glContext.vertexBufferObjectAvailable = R_CheckExtension( "GL_ARB_vertex_buffer_object" );
	if ( glContext.vertexBufferObjectAvailable ) {
		qglBindBufferARB = (PFNGLBINDBUFFERARBPROC)GLimp_ExtensionPointer( "glBindBufferARB" );
		qglBindBufferRange = (PFNGLBINDBUFFERRANGEPROC)GLimp_ExtensionPointer( "glBindBufferRange" );
		qglDeleteBuffersARB = (PFNGLDELETEBUFFERSARBPROC)GLimp_ExtensionPointer( "glDeleteBuffersARB" );
		qglGenBuffersARB = (PFNGLGENBUFFERSARBPROC)GLimp_ExtensionPointer( "glGenBuffersARB" );
		qglIsBufferARB = (PFNGLISBUFFERARBPROC)GLimp_ExtensionPointer( "glIsBufferARB" );
		qglBufferDataARB = (PFNGLBUFFERDATAARBPROC)GLimp_ExtensionPointer( "glBufferDataARB" );
		qglBufferSubDataARB = (PFNGLBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer( "glBufferSubDataARB" );
		qglGetBufferSubDataARB = (PFNGLGETBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer( "glGetBufferSubDataARB" );
		qglMapBufferARB = (PFNGLMAPBUFFERARBPROC)GLimp_ExtensionPointer( "glMapBufferARB" );
		qglUnmapBufferARB = (PFNGLUNMAPBUFFERARBPROC)GLimp_ExtensionPointer( "glUnmapBufferARB" );
		qglGetBufferParameterivARB = (PFNGLGETBUFFERPARAMETERIVARBPROC)GLimp_ExtensionPointer( "glGetBufferParameterivARB" );
		qglGetBufferPointervARB = (PFNGLGETBUFFERPOINTERVARBPROC)GLimp_ExtensionPointer( "glGetBufferPointervARB" );
	}

	// GL_ARB_map_buffer_range, map a section of a buffer object's data store
	glContext.mapBufferRangeAvailable = R_CheckExtension( "GL_ARB_map_buffer_range" );
	if ( glContext.mapBufferRangeAvailable ) {
		qglMapBufferRange = (PFNGLMAPBUFFERRANGEPROC)GLimp_ExtensionPointer( "glMapBufferRange" );
	}

	// GL_ARB_vertex_array_object
	glContext.vertexArrayObjectAvailable = R_CheckExtension( "GL_ARB_vertex_array_object" );
	if ( glContext.vertexArrayObjectAvailable ) {
		qglGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)GLimp_ExtensionPointer( "glGenVertexArrays" );
		qglBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)GLimp_ExtensionPointer( "glBindVertexArray" );
		qglDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)GLimp_ExtensionPointer( "glDeleteVertexArrays" );
	}

	// GL_ARB_draw_elements_base_vertex
	glContext.drawElementsBaseVertexAvailable = R_CheckExtension( "GL_ARB_draw_elements_base_vertex" );
	if ( glContext.drawElementsBaseVertexAvailable ) {
		qglDrawElementsBaseVertex = (PFNGLDRAWELEMENTSBASEVERTEXPROC)GLimp_ExtensionPointer( "glDrawElementsBaseVertex" );
	}

	// GL_ARB_vertex_program / GL_ARB_fragment_program
	glContext.fragmentProgramAvailable = R_CheckExtension( "GL_ARB_fragment_program" );
	if ( glContext.fragmentProgramAvailable ) {
		qglVertexAttribPointerARB = (PFNGLVERTEXATTRIBPOINTERARBPROC)GLimp_ExtensionPointer( "glVertexAttribPointerARB" );
		qglEnableVertexAttribArrayARB = (PFNGLENABLEVERTEXATTRIBARRAYARBPROC)GLimp_ExtensionPointer( "glEnableVertexAttribArrayARB" );
		qglDisableVertexAttribArrayARB = (PFNGLDISABLEVERTEXATTRIBARRAYARBPROC)GLimp_ExtensionPointer( "glDisableVertexAttribArrayARB" );

		qglGetIntegerv( GL_MAX_TEXTURE_COORDS_ARB, (GLint *)&glContext.maxTextureCoords );
		qglGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS_ARB, (GLint *)&glContext.maxTextureImageUnits );
	}

	// GLSL, core in OpenGL > 2.0
	glContext.glslAvailable = ( glContext.glVersion >= 2.0f );
	if ( glContext.glslAvailable ) {
		qglCreateShader = (PFNGLCREATESHADERPROC)GLimp_ExtensionPointer( "glCreateShader" );
		qglDeleteShader = (PFNGLDELETESHADERPROC)GLimp_ExtensionPointer( "glDeleteShader" );
		qglShaderSource = (PFNGLSHADERSOURCEPROC)GLimp_ExtensionPointer( "glShaderSource" );
		qglCompileShader = (PFNGLCOMPILESHADERPROC)GLimp_ExtensionPointer( "glCompileShader" );
		qglGetShaderiv = (PFNGLGETSHADERIVPROC)GLimp_ExtensionPointer( "glGetShaderiv" );
		qglGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)GLimp_ExtensionPointer( "glGetShaderInfoLog" );
		qglCreateProgram = (PFNGLCREATEPROGRAMPROC)GLimp_ExtensionPointer( "glCreateProgram" );
		qglDeleteProgram = (PFNGLDELETEPROGRAMPROC)GLimp_ExtensionPointer( "glDeleteProgram" );
		qglAttachShader = (PFNGLATTACHSHADERPROC)GLimp_ExtensionPointer( "glAttachShader" );
		qglLinkProgram = (PFNGLLINKPROGRAMPROC)GLimp_ExtensionPointer( "glLinkProgram" );
		qglUseProgram = (PFNGLUSEPROGRAMPROC)GLimp_ExtensionPointer( "glUseProgram" );
		qglGetProgramiv = (PFNGLGETPROGRAMIVPROC)GLimp_ExtensionPointer( "glGetProgramiv" );
		qglGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)GLimp_ExtensionPointer( "glGetProgramInfoLog" );
		qglBindAttribLocation = (PFNGLBINDATTRIBLOCATIONPROC)GLimp_ExtensionPointer( "glBindAttribLocation" );
		qglGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)GLimp_ExtensionPointer( "glGetUniformLocation" );
		qglUniform1i = (PFNGLUNIFORM1IPROC)GLimp_ExtensionPointer( "glUniform1i" );
		qglUniform4fv = (PFNGLUNIFORM4FVPROC)GLimp_ExtensionPointer( "glUniform4fv" );
	}

	// GL_ARB_uniform_buffer_object
	glContext.uniformBufferAvailable = R_CheckExtension( "GL_ARB_uniform_buffer_object" );
	if ( glContext.uniformBufferAvailable ) {
		qglGetUniformBlockIndex = (PFNGLGETUNIFORMBLOCKINDEXPROC)GLimp_ExtensionPointer( "glGetUniformBlockIndex" );
		qglUniformBlockBinding = (PFNGLUNIFORMBLOCKBINDINGPROC)GLimp_ExtensionPointer( "glUniformBlockBinding" );

		qglGetIntegerv( GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, (GLint *)&glContext.uniformBufferOffsetAlignment );
		if ( glContext.uniformBufferOffsetAlignment < 256 ) {
			glContext.uniformBufferOffsetAlignment = 256;
		}
	}

	// ATI_separate_stencil / OpenGL 2.0 separate stencil
	glContext.twoSidedStencilAvailable = ( glContext.glVersion >= 2.0f ) || R_CheckExtension( "GL_ATI_separate_stencil" );
	if ( glContext.twoSidedStencilAvailable ) {
		qglStencilOpSeparate = (PFNGLSTENCILOPSEPARATEATIPROC)GLimp_ExtensionPointer( "glStencilOpSeparate" );
		qglStencilFuncSeparate = (PFNGLSTENCILFUNCSEPARATEATIPROC)GLimp_ExtensionPointer( "glStencilFuncSeparate" );
	}

	// GL_EXT_depth_bounds_test
 	glContext.depthBoundsTestAvailable = R_CheckExtension( "GL_EXT_depth_bounds_test" );
 	if ( glContext.depthBoundsTestAvailable ) {
 		qglDepthBoundsEXT = (PFNGLDEPTHBOUNDSEXTPROC)GLimp_ExtensionPointer( "glDepthBoundsEXT" );
 	}

	// GL_ARB_sync
	glContext.syncAvailable = R_CheckExtension( "GL_ARB_sync" ) &&
		// as of 5/24/2012 (driver version 15.26.12.64.2761) sync objects
		// do not appear to work for the Intel HD 4000 graphics
		( glContext.vendor != VENDOR_INTEL || r_skipIntelWorkarounds.GetBool() );
	if ( glContext.syncAvailable ) {
		qglFenceSync = (PFNGLFENCESYNCPROC)GLimp_ExtensionPointer( "glFenceSync" );
		qglIsSync = (PFNGLISSYNCPROC)GLimp_ExtensionPointer( "glIsSync" );
		qglClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)GLimp_ExtensionPointer( "glClientWaitSync" );
		qglDeleteSync = (PFNGLDELETESYNCPROC)GLimp_ExtensionPointer( "glDeleteSync" );
	}

	// GL_ARB_occlusion_query
	glContext.occlusionQueryAvailable = R_CheckExtension( "GL_ARB_occlusion_query" );
	if ( glContext.occlusionQueryAvailable ) {
		// defined in GL_ARB_occlusion_query, which is required for GL_EXT_timer_query
		qglGenQueriesARB = (PFNGLGENQUERIESARBPROC)GLimp_ExtensionPointer( "glGenQueriesARB" );
		qglDeleteQueriesARB = (PFNGLDELETEQUERIESARBPROC)GLimp_ExtensionPointer( "glDeleteQueriesARB" );
		qglIsQueryARB = (PFNGLISQUERYARBPROC)GLimp_ExtensionPointer( "glIsQueryARB" );
		qglBeginQueryARB = (PFNGLBEGINQUERYARBPROC)GLimp_ExtensionPointer( "glBeginQueryARB" );
		qglEndQueryARB = (PFNGLENDQUERYARBPROC)GLimp_ExtensionPointer( "glEndQueryARB" );
		qglGetQueryivARB = (PFNGLGETQUERYIVARBPROC)GLimp_ExtensionPointer( "glGetQueryivARB" );
		qglGetQueryObjectivARB = (PFNGLGETQUERYOBJECTIVARBPROC)GLimp_ExtensionPointer( "glGetQueryObjectivARB" );
		qglGetQueryObjectuivARB = (PFNGLGETQUERYOBJECTUIVARBPROC)GLimp_ExtensionPointer( "glGetQueryObjectuivARB" );
	}

	// GL_ARB_timer_query
	const bool arbTimerQueryAvailable = R_CheckExtension( "GL_ARB_timer_query" );
	glContext.timerQueryAvailable = arbTimerQueryAvailable || R_CheckExtension( "GL_EXT_timer_query" );
	if ( glContext.timerQueryAvailable ) {
		qglGetQueryObjectui64vEXT = (PFNGLGETQUERYOBJECTUI64VEXTPROC)GLimp_ExtensionPointer( "glGetQueryObjectui64vARB" );
		if ( qglGetQueryObjectui64vEXT == NULL ) {
			qglGetQueryObjectui64vEXT = (PFNGLGETQUERYOBJECTUI64VEXTPROC)GLimp_ExtensionPointer( "glGetQueryObjectui64vEXT" );
		}
	}

	// GL_TIMESTAMP queries, only in GL_ARB_timer_query: r_pix times the render passes with them
	qglQueryCounter = nullptr;
	if ( arbTimerQueryAvailable && glContext.occlusionQueryAvailable && qglGetQueryObjectui64vEXT != nullptr ) {
		qglQueryCounter = (PFNGLQUERYCOUNTERPROC)GLimp_ExtensionPointer( "glQueryCounter" );
	}
	glContext.timestampQueryAvailable = ( qglQueryCounter != nullptr );

	// GL_KHR_debug: r_pix 2 names the render passes for RenderDoc, Radeon GPU Profiler and Nsight
	qglPushDebugGroup = nullptr;
	qglPopDebugGroup = nullptr;
	if ( R_CheckExtension( "GL_KHR_debug" ) || glContext.glVersion >= 4.3f ) {
		qglPushDebugGroup = (PFNGLPUSHDEBUGGROUPPROC)GLimp_ExtensionPointer( "glPushDebugGroup" );
		qglPopDebugGroup = (PFNGLPOPDEBUGGROUPPROC)GLimp_ExtensionPointer( "glPopDebugGroup" );
	}
	glContext.debugGroupsAvailable = ( qglPushDebugGroup != nullptr && qglPopDebugGroup != nullptr );

	// GL_ARB_debug_output
	glContext.debugOutputAvailable = R_CheckExtension( "GL_ARB_debug_output" );
	if ( glContext.debugOutputAvailable ) {
		qglDebugMessageControlARB   = (PFNGLDEBUGMESSAGECONTROLARBPROC)GLimp_ExtensionPointer( "glDebugMessageControlARB" );
		qglDebugMessageInsertARB    = (PFNGLDEBUGMESSAGEINSERTARBPROC)GLimp_ExtensionPointer( "glDebugMessageInsertARB" );
		qglDebugMessageCallbackARB  = (PFNGLDEBUGMESSAGECALLBACKARBPROC)GLimp_ExtensionPointer( "glDebugMessageCallbackARB" );
		qglGetDebugMessageLogARB    = (PFNGLGETDEBUGMESSAGELOGARBPROC)GLimp_ExtensionPointer( "glGetDebugMessageLogARB" );

		if ( r_debugContext.GetInteger() >= 1 ) {
			qglDebugMessageCallbackARB( DebugCallback, NULL );
		}
		if ( r_debugContext.GetInteger() >= 2 ) {
			// force everything to happen in the main thread instead of in a separate driver thread
			glEnable( GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB );
		}
		if ( r_debugContext.GetInteger() >= 3 ) {
			// enable all the low priority messages
			qglDebugMessageControlARB( GL_DONT_CARE,
									GL_DONT_CARE,
									GL_DEBUG_SEVERITY_LOW_ARB,
									0, NULL, true );
		}
	}

	// GL_ARB_multitexture
	if ( !glContext.multitextureAvailable ) {
		idLib::Error( "GL_ARB_multitexture not available" );
	}
	// GL_ARB_texture_compression + GL_EXT_texture_compression_s3tc
	if ( !glContext.textureCompressionAvailable ) {
		idLib::Error( "GL_ARB_texture_compression or GL_EXT_texture_compression_s3tc not available" );
	}
	// GL_ARB_vertex_buffer_object
	if ( !glContext.vertexBufferObjectAvailable ) {
		idLib::Error( "GL_ARB_vertex_buffer_object not available" );
	}
	// GL_ARB_map_buffer_range
	if ( !glContext.mapBufferRangeAvailable ) {
		idLib::Error( "GL_ARB_map_buffer_range not available" );
	}
	// GL_ARB_vertex_array_object
	if ( !glContext.vertexArrayObjectAvailable ) {
		idLib::Error( "GL_ARB_vertex_array_object not available" );
	}
	// GL_ARB_draw_elements_base_vertex
	if ( !glContext.drawElementsBaseVertexAvailable ) {
		idLib::Error( "GL_ARB_draw_elements_base_vertex not available" );
	}
	// GL_ARB_vertex_program / GL_ARB_fragment_program
	if ( !glContext.fragmentProgramAvailable ) {
		idLib::Error( "GL_ARB_fragment_program not available" );
	}
	// GLSL
	if ( !glContext.glslAvailable ) {
		idLib::Error( "GLSL not available" );
	}
	// GL_ARB_uniform_buffer_object
	if ( !glContext.uniformBufferAvailable ) {
		idLib::Error( "GL_ARB_uniform_buffer_object not available" );
	}
	// GL_EXT_stencil_two_side
	if ( !glContext.twoSidedStencilAvailable ) {
		idLib::Error( "GL_ATI_separate_stencil not available" );
	}

	// generate one global Vertex Array Object (VAO)
	qglGenVertexArrays( 1, &glContext.global_vao );
	qglBindVertexArray( glContext.global_vao );

}
