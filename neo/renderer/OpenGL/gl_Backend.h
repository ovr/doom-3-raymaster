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

#ifndef __GL_BACKEND_H__
#define __GL_BACKEND_H__

/*
===============================================================================

	idRenderBackendGL: the OpenGL implementation of idRenderBackend.

	Private to renderer/OpenGL/: shared code reaches it only through renderBackend.
	A method whose body has not moved here yet stops with a fatal error.

===============================================================================
*/

enum graphicsVendor_t {
	VENDOR_NVIDIA,
	VENDOR_AMD,
	VENDOR_INTEL
};

// What the OpenGL context can do, for OpenGL code only: shared code reads renderBackend->Caps().
// Constant between idRenderBackendGL::Init and Shutdown.
struct glContext_t {
	const char *		renderer_string;
	const char *		vendor_string;
	const char *		version_string;
	const char *		extensions_string;
	const char *		wgl_extensions_string;
	const char *		shading_language_string;

	float				glVersion;				// atof( version_string )
	graphicsVendor_t	vendor;

	int					maxTextureSize;			// queried from GL
	int					maxTextureCoords;
	int					maxTextureImageUnits;
	int					uniformBufferOffsetAlignment;
	float				maxTextureAnisotropy;

	int					colorBits;
	int					depthBits;
	int					stencilBits;

	bool				multitextureAvailable;
	bool				directStateAccess;
	bool				textureCompressionAvailable;
	bool				anisotropicFilterAvailable;
	bool				textureLODBiasAvailable;
	bool				seamlessCubeMapAvailable;
	bool				sRGBFramebufferAvailable;
	bool				vertexBufferObjectAvailable;
	bool				mapBufferRangeAvailable;
	bool				vertexArrayObjectAvailable;
	bool				drawElementsBaseVertexAvailable;
	bool				fragmentProgramAvailable;
	bool				glslAvailable;
	bool				uniformBufferAvailable;
	bool				twoSidedStencilAvailable;
	bool				depthBoundsTestAvailable;
	bool				syncAvailable;
	bool				timerQueryAvailable;
	bool				timestampQueryAvailable;	// glQueryCounter( GL_TIMESTAMP ), GL_ARB_timer_query only
	bool				occlusionQueryAvailable;
	bool				debugOutputAvailable;
	bool				debugGroupsAvailable;		// glPushDebugGroup, GL_KHR_debug
	bool				swapControlTearAvailable;

	bool				isStereoPixelFormat;
	bool				stereoPixelFormatAvailable;
	int					multisamples;

	GLuint				global_vao;
};

extern glContext_t		glContext;

// gl_Extensions.cpp: loads the qgl* extension pointers, fills the flags of glContext and creates global_vao
void					R_CheckPortableExtensions();

class idRenderBackendGL final : public idRenderBackend {
public:
						idRenderBackendGL();

	bool				Init( const renderBackendInit_t & init ) override;
	void				Shutdown() override;
	void				ResizeOutput( int width, int height ) override;
	void				SetOption( renderOption_t option, bool enable ) override;
	void				SetGammaRamp( const unsigned short red[256], const unsigned short green[256], const unsigned short blue[256] ) override;
	void				CheckErrors() override;
	void				PrintInfo() const override;

	void				BeginFrame() override;
	void				EndFrame() override;
	presentResult_t		Present( presentMode_t mode ) override;
	uint64				GetFrameGpuMicroseconds() override;
	void				SelectOutput( outputEye_t eye ) override;
	void				ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) override;

	textureHandle_t		CreateTexture( const idImageOpts & opts, textureRole_t role, const samplerDesc_t & sampler, const char * debugName ) override;
	void				UploadTexture( textureHandle_t texture, int mipLevel, int face, const void * pixels, int pixelPitch ) override;
	void				SetTextureSampler( textureHandle_t texture, const samplerDesc_t & sampler ) override;
	void				CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) override;
	void				CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height ) override;

	bufferHandle_t		CreateBuffer( bufferType_t type, bufferUsage_t usage, int size, const void * data ) override;
	void				DestroyBuffer( bufferHandle_t buffer ) override;
	void *				MapBuffer( bufferHandle_t buffer ) override;
	void				UnmapBuffer( bufferHandle_t buffer, int dirtyOffset, int dirtyBytes ) override;

	shaderHandle_t		CreateShaderStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info ) override;
	programHandle_t		CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * debugName, programInfo_t & info ) override;
	void				DestroyShaderStage( shaderHandle_t shader ) override;
	void				DestroyProgram( programHandle_t program ) override;

	void				WriteTimestamp( int set, int index ) override;
	bool				TimestampReady( int set, int index ) override;
	void				ReadTimestamps( int set, int count, uint64 * nanoseconds ) override;
	void				FreeTimestamps() override;
	void				PushDebugGroup( const char * name ) override;
	void				PopDebugGroup() override;

protected:
	void				API_SetDefaultState() override;
	void				API_State( uint64 stateBits, uint64 diff ) override;
	void				API_BeginView() override;
	void				API_Cull( int cullType, int previousCullType ) override;
	void				API_PolygonOffset( float scale, float bias ) override;
	void				API_DepthBoundsTest( float zmin, float zmax ) override;
	void				API_Viewport( const renderRect_t & rect ) override;
	void				API_Scissor( const renderRect_t & rect ) override;
	void				API_BindTexture( int slot, textureKind_t kind, textureHandle_t texture ) override;
	void				API_BindProgram( programHandle_t program ) override;
	void				API_Clear( const clearDesc_t & clear ) override;
	void				API_DestroyTexture( textureHandle_t texture ) override;
	void				API_DrawIndexed( const drawCall_t & dc ) override;
};

#endif /* !__GL_BACKEND_H__ */
