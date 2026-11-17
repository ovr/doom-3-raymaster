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
	A bufferHandle_t, textureHandle_t, shaderHandle_t or programHandle_t is a pointer to a record below.

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

// What a textureHandle_t points at.
struct glTexture_t {
	GLuint				texnum;				// gl texture binding
	GLenum				target;				// GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP_EXT
	// we could derive these in UploadTexture each time if necessary
	GLuint				internalFormat;
	GLuint				dataFormat;
	GLuint				dataType;
	idImageOpts			opts;				// the backend's copy: a copy from the output changes its size
#ifdef _DEBUG
	idStr				name;
	int					drawnInFrame;		// the frame of the last draw whose program samples it
#endif
};

ID_INLINE glTexture_t * GL_Texture( textureHandle_t texture ) {
	return reinterpret_cast<glTexture_t *>( static_cast<uintptr_t>( texture ) );
}
ID_INLINE textureHandle_t GL_TextureHandle( glTexture_t * texture ) {
	return static_cast<textureHandle_t>( reinterpret_cast<uintptr_t>( texture ) );
}

// What a shaderHandle_t points at.
struct glShaderStage_t {
	GLuint				shader;				// 0 when the source file is missing: such a stage is never loaded again
	shaderStageInfo_t	info;
	idStr				name;
};

// What a programHandle_t points at.
struct glProgram_t {
	GLuint				progId;
	GLint				vertexUniformArray;		// location of the vec4 array of the stage, -1 when it has none
	GLint				fragmentUniformArray;
	shaderStageInfo_t	vertexInfo;				// copies: a stage can be destroyed before its program
	shaderStageInfo_t	fragmentInfo;
#ifdef _DEBUG
	programInfo_t		info;					// what CreateProgram returned, for the draw checks
	idStr				name;
	uint16				warnedSamplers;			// slots and vertex layouts already reported, bit per each
	uint8				warnedLayouts;
	bool				warnedJoints;
#endif
};

ID_INLINE glShaderStage_t * GL_ShaderStage( shaderHandle_t shader ) {
	return reinterpret_cast<glShaderStage_t *>( static_cast<uintptr_t>( shader ) );
}
ID_INLINE glProgram_t * GL_Program( programHandle_t program ) {
	return reinterpret_cast<glProgram_t *>( static_cast<uintptr_t>( program ) );
}

// What a bufferHandle_t points at.
struct glBuffer_t {
	GLuint				bufferObject;
	GLenum				target;				// GL_ARRAY_BUFFER_ARB, GL_ELEMENT_ARRAY_BUFFER_ARB or GL_UNIFORM_BUFFER
	int					size;				// bytes
};

ID_INLINE glBuffer_t * GL_Buffer( bufferHandle_t buffer ) {
	return reinterpret_cast<glBuffer_t *>( static_cast<uintptr_t>( buffer ) );
}
ID_INLINE bufferHandle_t GL_BufferHandle( glBuffer_t * buffer ) {
	return static_cast<bufferHandle_t>( reinterpret_cast<uintptr_t>( buffer ) );
}

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
	void				UploadTexture( textureHandle_t texture, int mipLevel, int face, int width, int height, const void * pixels, int pixelPitch ) override;
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

private:
	// Sends the parms both stages of the program read to the program GL has in use.
	void				CommitParms( programHandle_t program, const idVec4 * parms );

	// r_glCheckLatch (gl_Draw.cpp): GL's culling and depth function against the latch rules
	void				CheckLatch();
	void				ReportLatchChecks();
	static constexpr int MAX_LATCH_WARNINGS = 20;
	int					latchChecks = 0;
	int					latchMismatches = 0;

	// what API_DrawIndexed left bound, so that a repeat is skipped; reset by API_SetDefaultState
	vertexLayoutType_t	currentVertexLayout = LAYOUT_UNKNOWN;
	bufferHandle_t		currentVertexBuffer = bufferHandle_t::NONE;
	bufferHandle_t		currentIndexBuffer = bufferHandle_t::NONE;

	GLuint				timerQueryId = 0;			// the GL_TIME_ELAPSED_EXT query around a frame
	idList<GLuint>		timestampQueries[TIMESTAMP_SETS];	// r_pix: slot index of a set is its query

	int					swapIndex = 0;				// 0 or 1 into renderSync
	GLsync				renderSync[2] = {};
	presentMode_t		swapMode = presentMode_t::IMMEDIATE;	// what the swap interval was set from
	bool				swapModeSet = false;

	int					debugFrame = 0;				// Debug: counts BeginFrame
	int					debugUploadsAfterDraw = 0;	// Debug: uploads into a texture the frame had drawn with already (rule R4)
#ifdef _DEBUG
	void				DebugCheckDraw( const drawCall_t & dc );

	// the packed parms of the last draw that sent them, for the reuseParms promise (rule R7)
	idVec4				debugLastParms[2][MAX_RENDER_PARMS];
	int					debugLastNumParms[2] = {};
	int					debugParmsReuseBroken = 0;
	programHandle_t		debugLastProgram = programHandle_t::NONE;
	uint32				debugTexturesBound = 0;		// bit slot * 2 + kind: API_BindTexture since API_SetDefaultState
#endif
};

#endif /* !__GL_BACKEND_H__ */
