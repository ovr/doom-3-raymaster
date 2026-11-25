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

#ifndef __RENDERBACKENDTYPES_H__
#define __RENDERBACKENDTYPES_H__

/*
===============================================================================

	Types of the graphics-API-neutral render interface (RenderBackend.h).

	Depends only on idLib base types, so the resource headers that enter the
	precompiled header before tr_local.h can include it.

	A handle is a pointer to a backend record dressed as an enum class. Shared
	code never looks inside and compares it only with NONE or another handle.

===============================================================================
*/

#include "RenderParms.h"

enum class textureHandle_t : uintptr_t { NONE = 0 };
enum class bufferHandle_t : uintptr_t { NONE = 0 };
enum class shaderHandle_t : uintptr_t { NONE = 0 };
enum class programHandle_t : uintptr_t { NONE = 0 };

enum class bufferType_t : uint8 { VERTEX, INDEX, JOINT };
enum class bufferUsage_t : uint8 {
	STATIC,		// written rarely, in growing ranges; lives as long as the level
	FRAME		// remapped whole every frame; written by the front end while the previous frame draws from another buffer
};
// not shaderStage_t: that is a material stage (Material.h). COMPUTE: only a backend's own passes, no render program.
enum class shaderStageType_t : uint8 { VERTEX, FRAGMENT, COMPUTE };
enum class primitive_t : uint8 { TRIANGLES, LINES };
enum class textureKind_t : uint8 { TEX_2D, TEX_CUBE };
enum class textureRole_t : uint8 {
	SAMPLED,		// filled by UploadTexture
	COLOR_COPY,		// also the target of CopyOutputToTexture
	DEPTH_COPY		// the target of CopyDepthToTexture; sampled as one float channel in 0..1
};
enum class outputEye_t : uint8 { BOTH, LEFT, RIGHT };
// r_swapInterval 0, 1, 2; NONE finishes the frame without showing it
enum class presentMode_t : uint8 { IMMEDIATE, ADAPTIVE, VSYNC, NONE };
enum class presentResult_t : uint8 { OK, DEVICE_LOST };
enum class renderOption_t : uint8 { SRGB_OUTPUT, SEAMLESS_CUBE_MAPS, MULTISAMPLE };	// hints: a backend without the notion ignores them
enum class samplerFilter_t : uint8 { NEAREST, LINEAR, LINEAR_MIP_NEAREST, LINEAR_MIP_LINEAR };
enum class samplerAddress_t : uint8 { REPEAT, CLAMP, BORDER_BLACK, BORDER_TRANSPARENT };	// border 0,0,0,1 and 0,0,0,0

constexpr int MAX_TEXTURE_SLOTS	= 8;		// texture units a draw can sample
constexpr int MAX_RENDER_PARMS	= 136;		// RENDERPARM_USER + MAX_GLSL_USER_PARMS; RenderProgs.h checks it with a static_assert
constexpr int TIMESTAMP_SETS	= 4;
constexpr int CULL_UNSET		= -1;		// no Cull() since SetDefaultState or SetViewMirror

struct renderRect_t {
	int		x, y, width, height;	// output pixels, origin at the bottom left, like idScreenRect
};

// fully resolved by idImage::ResolveSampler; a backend never reads a filter cvar
struct samplerDesc_t {
	samplerFilter_t		filter;
	samplerAddress_t	address;
	float				maxAnisotropy;	// 1 = off, already clamped to caps.maxAnisotropy
	float				lodBias;
	bool				operator==( const samplerDesc_t & ) const = default;
};

enum vertexLayoutType_t : uint8 {
	LAYOUT_UNKNOWN,
	LAYOUT_DRAW_VERT,					// idDrawVert, 32 bytes
	LAYOUT_DRAW_SHADOW_VERT,			// idShadowVert, 16 bytes
	LAYOUT_DRAW_SHADOW_VERT_SKINNED,	// idShadowVertSkinned, 32 bytes
	NUM_VERTEX_LAYOUTS
};
enum class vertexAttribFormat_t : uint8 { FLOAT3, FLOAT4, HALF2, UBYTE4_NORM };
struct vertexAttribDesc_t {
	uint8					index;		// PC_ATTRIB_INDEX_*
	vertexAttribFormat_t	format;
	uint8					offset;
};
struct vertexLayoutDesc_t {
	uint8				stride;
	uint8				numAttribs;
	uint16				attribMask;		// bit per PC_ATTRIB_INDEX_*
	vertexAttribDesc_t	attribs[6];
};

static_assert( sizeof( idDrawVert ) == 32 && sizeof( idShadowVert ) == 16 && sizeof( idShadowVertSkinned ) == 32 );

// The vertex inputs of every layout, in the order the OpenGL path sets them up.
inline constexpr vertexLayoutDesc_t vertexLayouts[NUM_VERTEX_LAYOUTS] = {
	// LAYOUT_UNKNOWN
	{},
	// LAYOUT_DRAW_VERT
	{
		sizeof( idDrawVert ), 6,
		( 1 << PC_ATTRIB_INDEX_VERTEX ) | ( 1 << PC_ATTRIB_INDEX_NORMAL ) | ( 1 << PC_ATTRIB_INDEX_COLOR ) | ( 1 << PC_ATTRIB_INDEX_COLOR2 )
			| ( 1 << PC_ATTRIB_INDEX_ST ) | ( 1 << PC_ATTRIB_INDEX_TANGENT ),
		{
			{ PC_ATTRIB_INDEX_VERTEX,	vertexAttribFormat_t::FLOAT3,		DRAWVERT_XYZ_OFFSET },
			{ PC_ATTRIB_INDEX_NORMAL,	vertexAttribFormat_t::UBYTE4_NORM,	DRAWVERT_NORMAL_OFFSET },
			{ PC_ATTRIB_INDEX_COLOR,	vertexAttribFormat_t::UBYTE4_NORM,	DRAWVERT_COLOR_OFFSET },
			{ PC_ATTRIB_INDEX_COLOR2,	vertexAttribFormat_t::UBYTE4_NORM,	DRAWVERT_COLOR2_OFFSET },
			{ PC_ATTRIB_INDEX_ST,		vertexAttribFormat_t::HALF2,		DRAWVERT_ST_OFFSET },
			{ PC_ATTRIB_INDEX_TANGENT,	vertexAttribFormat_t::UBYTE4_NORM,	DRAWVERT_TANGENT_OFFSET }
		}
	},
	// LAYOUT_DRAW_SHADOW_VERT
	{
		sizeof( idShadowVert ), 1,
		1 << PC_ATTRIB_INDEX_VERTEX,
		{
			{ PC_ATTRIB_INDEX_VERTEX,	vertexAttribFormat_t::FLOAT4,		SHADOWVERT_XYZW_OFFSET }
		}
	},
	// LAYOUT_DRAW_SHADOW_VERT_SKINNED
	{
		sizeof( idShadowVertSkinned ), 3,
		( 1 << PC_ATTRIB_INDEX_VERTEX ) | ( 1 << PC_ATTRIB_INDEX_COLOR ) | ( 1 << PC_ATTRIB_INDEX_COLOR2 ),
		{
			{ PC_ATTRIB_INDEX_VERTEX,	vertexAttribFormat_t::FLOAT4,		SHADOWVERTSKINNED_XYZW_OFFSET },
			{ PC_ATTRIB_INDEX_COLOR,	vertexAttribFormat_t::UBYTE4_NORM,	SHADOWVERTSKINNED_COLOR_OFFSET },
			{ PC_ATTRIB_INDEX_COLOR2,	vertexAttribFormat_t::UBYTE4_NORM,	SHADOWVERTSKINNED_COLOR2_OFFSET }
		}
	}
};

struct renderCaps_t {
	const char *	apiName;				// "OpenGL", "Direct3D 12", "Vulkan"
	const char *	vendorName;
	const char *	rendererName;
	const char *	versionString;
	int				maxTextureSize;
	float			maxAnisotropy;			// 1 when anisotropic filtering is missing
	int				uniformBufferOffsetAlignment;	// joint offsets are multiples of this; at most 256
	int				maxSamples;
	int				depthBits;				// printed by gfxInfo only
	int				maxDebugGroupDepth;
	bool			depthBoundsTest;
	bool			maskedColorClear;		// Clear() can honour a partial colour write mask
	bool			quadBufferStereo;		// the display has a quad-buffer format; SelectOutput( LEFT / RIGHT ) also needs Init to have asked for it
	bool			adaptiveSync;			// presentMode_t::ADAPTIVE differs from VSYNC
	bool			gpuTimer;				// GetFrameGpuMicroseconds returns a measurement
	bool			timestamps;				// WriteTimestamp works
	bool			debugGroups;
	bool			hdrScene;				// BeginHdrScene .. ResolveHdrScene draw into a linear float scene
	bool			rayTracing;				// inline ray queries against the GPU scene (DXR 1.1)
	bool			gbuffer;				// BeginGBuffer .. EndGBuffer write the thin G-buffer (R14); needs hdrScene
};

struct renderBackendInit_t {
	void *		nativeWindow;		// HWND
	int			width, height;		// client area
	int			multiSamples;		// fixed until Shutdown
	bool		stereoPixelFormat;	// ask for quad-buffer; Init fails when it cannot be had
};

// what a stage reads; filled by CreateShaderStage
struct shaderStageInfo_t {
	int			numParms;
	uint8		parms[MAX_RENDER_PARMS];	// renderParm_t indexes in the order of the stage's constant block
};
// what a linked program needs from a draw; filled by CreateProgram
struct programInfo_t {
	uint16		samplerMask;		// texture slots it samples
	uint16		cubeMask;			// those of them that are cube maps
	uint16		attribMask;			// PC_ATTRIB_INDEX_* inputs of the vertex stage
	bool		usesJoints;			// reads the joint block
};

struct drawCall_t {
	bufferHandle_t		vertexBuffer;
	bufferHandle_t		indexBuffer;
	bufferHandle_t		jointBuffer;	// NONE = unskinned
	int					baseVertex;		// added to every index
	int					indexOffset;	// bytes into indexBuffer; indexes are triIndex_t
	int					numIndexes;
	int					jointOffset;	// bytes, a multiple of caps.uniformBufferOffsetAlignment
	int					numJoints;
	vertexLayoutType_t	layout;
	primitive_t			topology;
	bool				reuseParms;		// same program and same parm values as the previous DrawIndexed
	const idVec4 *		parms;			// MAX_RENDER_PARMS vectors, read during the call only
};

struct clearDesc_t {
	renderRect_t	rect;				// the latched scissor
	bool			color, depth, stencil;
	uint8			colorWriteMask;		// bit 0 red .. bit 3 alpha, from the latched GLS_*MASK bits
	uint8			stencilValue;
	float			rgba[4];
};

// the 16 bytes a pipeline object is looked up by; GL never builds one
struct pipelineKey_t {
	uint64		stateBits;		// latch.stateBits & GLS_PIPELINE_BITS
	uint16		program;		// the backend's dense program index
	uint8		layout;			// vertexLayoutType_t
	uint8		raster;			// bits 0-1 effective cull (0 none, 1 front, 2 back), bit 2 lines, bits 3-5 target format id,
							// bit 6 strict depth test (LESS where the GLS bits say LEQUAL), see renderLatch_t
	uint8		depthBias;		// 0 = off or dynamic, else slot + 1 in the backend's bias table
	uint8		pad[3];
	bool		operator==( const pipelineKey_t & ) const = default;
};
static_assert( sizeof( pipelineKey_t ) == 16 );

// what shared code asked for; the only state a draw depends on besides drawCall_t
struct renderLatch_t {
	uint64				stateBits = 0;
	int					cull = CULL_UNSET;		// cullType_t
	bool				viewIsMirror = false;
	// GL's rules the frames depend on, both reset by SetDefaultState: TWO_SIDED turns culling off and only a
	// one-sided Cull right after a TWO_SIDED one turns it on (not one after SetViewMirror's CULL_UNSET); the
	// depth test is strict LESS until a State whose changed bits include GLS_DEPTHFUNC_BITS.
	bool				cullEnabled = true;
	bool				depthFuncStrict = true;
	float				polyOfsScale = 0.0f;
	float				polyOfsBias = 0.0f;
	float				depthBoundsMin = 0.0f;	// 0, 0 = test off
	float				depthBoundsMax = 0.0f;
	renderRect_t		viewport = {};
	renderRect_t		scissor = {};
	programHandle_t		program = programHandle_t::NONE;
	textureHandle_t		textures[MAX_TEXTURE_SLOTS][2] = {};	// [slot][textureKind_t]; what a draw samples
	uint32				texturesSent = 0;		// bit slot * 2 + kind: API_BindTexture has seen this entry
};

// packs the parms a stage reads, in its order; the only implementation of constant packing
ID_INLINE int R_PackRenderParms( idVec4 * dst, const idVec4 * parms, const shaderStageInfo_t & info ) {
	for ( int i = 0; i < info.numParms; i++ ) {
		dst[i] = parms[info.parms[i]];
	}
	return info.numParms;
}

#endif /* !__RENDERBACKENDTYPES_H__ */
