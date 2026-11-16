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
#include "../idlib/precompiled.h"

#include "tr_local.h"

idRenderBackend *	renderBackend = nullptr;

idCVar r_renderApi( "r_renderApi", "opengl", CVAR_RENDERER | CVAR_INIT | CVAR_ARCHIVE, "graphics API of the renderer, read once at startup" );

idRenderBackendFactory * idRenderBackendFactory::staticFactories = nullptr;

/*
========================
vertexLayouts

The vertex inputs of every layout, in the order the OpenGL path sets them up.
========================
*/
namespace {

constexpr uint16 AttribBit( int index ) {
	return static_cast<uint16>( 1 << index );
}

}	// namespace

static_assert( sizeof( idDrawVert ) == 32 && sizeof( idShadowVert ) == 16 && sizeof( idShadowVertSkinned ) == 32 );

const vertexLayoutDesc_t vertexLayouts[NUM_VERTEX_LAYOUTS] = {
	// LAYOUT_UNKNOWN
	{},
	// LAYOUT_DRAW_VERT
	{
		sizeof( idDrawVert ), 6,
		AttribBit( PC_ATTRIB_INDEX_VERTEX ) | AttribBit( PC_ATTRIB_INDEX_NORMAL ) | AttribBit( PC_ATTRIB_INDEX_COLOR )
			| AttribBit( PC_ATTRIB_INDEX_COLOR2 ) | AttribBit( PC_ATTRIB_INDEX_ST ) | AttribBit( PC_ATTRIB_INDEX_TANGENT ),
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
		AttribBit( PC_ATTRIB_INDEX_VERTEX ),
		{
			{ PC_ATTRIB_INDEX_VERTEX,	vertexAttribFormat_t::FLOAT4,		SHADOWVERT_XYZW_OFFSET }
		}
	},
	// LAYOUT_DRAW_SHADOW_VERT_SKINNED
	{
		sizeof( idShadowVertSkinned ), 3,
		AttribBit( PC_ATTRIB_INDEX_VERTEX ) | AttribBit( PC_ATTRIB_INDEX_COLOR ) | AttribBit( PC_ATTRIB_INDEX_COLOR2 ),
		{
			{ PC_ATTRIB_INDEX_VERTEX,	vertexAttribFormat_t::FLOAT4,		SHADOWVERTSKINNED_XYZW_OFFSET },
			{ PC_ATTRIB_INDEX_COLOR,	vertexAttribFormat_t::UBYTE4_NORM,	SHADOWVERTSKINNED_COLOR_OFFSET },
			{ PC_ATTRIB_INDEX_COLOR2,	vertexAttribFormat_t::UBYTE4_NORM,	SHADOWVERTSKINNED_COLOR2_OFFSET }
		}
	}
};

/*
========================
idRenderBackendFactory::idRenderBackendFactory

Runs before main: only links the instance into the list.
========================
*/
idRenderBackendFactory::idRenderBackendFactory( const char * name, idRenderBackend * ( *createBackend )() ) :
	apiName( name ),
	create( createBackend ),
	next( staticFactories ) {
	staticFactories = this;
}

/*
========================
idRenderBackendFactory::Create
========================
*/
idRenderBackend * idRenderBackendFactory::Create( const char * apiName ) {
	for ( const idRenderBackendFactory * factory = staticFactories; factory != nullptr; factory = factory->next ) {
		if ( idStr::Icmp( factory->apiName, apiName ) == 0 ) {
			return factory->create();
		}
	}
	return nullptr;
}

/*
========================
idRenderBackendFactory::ListApis
========================
*/
void idRenderBackendFactory::ListApis( idStrList & names ) {
	for ( const idRenderBackendFactory * factory = staticFactories; factory != nullptr; factory = factory->next ) {
		names.Append( factory->apiName );
	}
}

/*
========================
idRenderBackend::SetDefaultState

Once per frame (per eye in stereo). An immediate backend puts the API into this state in its hook.
========================
*/
void idRenderBackend::SetDefaultState() {
	latch.stateBits = 0;
	latch.cull = CULL_UNSET;
	latch.polyOfsScale = latch.polyOfsBias = 0.0f;
	latch.depthBoundsMin = latch.depthBoundsMax = 0.0f;
	latch.viewport = latch.scissor = { 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() };
	latch.texturesSent = 0;
	API_SetDefaultState();
}

/*
========================
idRenderBackend::Clear
========================
*/
void idRenderBackend::Clear( bool color, bool depth, bool stencil, byte stencilValue, float r, float g, float b, float a ) {
	clearDesc_t clear = {};
	clear.rect = latch.scissor;
	clear.color = color;
	clear.depth = depth;
	clear.stencil = stencil;
	clear.colorWriteMask = static_cast<uint8>( ( ~( latch.stateBits >> 7 ) ) & 0xF );	// GLS_REDMASK is bit 7, the masks are inverse
	clear.stencilValue = stencilValue;
	clear.rgba[0] = r;
	clear.rgba[1] = g;
	clear.rgba[2] = b;
	clear.rgba[3] = a;
	assert( !depth || ( latch.stateBits & GLS_DEPTHMASK ) == 0 );
	assert( !color || clear.colorWriteMask == 0xF || caps.maskedColorClear );
	API_Clear( clear );
}

/*
========================
idRenderBackend::DestroyTexture
========================
*/
void idRenderBackend::DestroyTexture( textureHandle_t texture ) {
	for ( auto & slot : latch.textures ) {
		for ( auto & entry : slot ) {
			if ( entry == texture ) {
				entry = textureHandle_t::NONE;
			}
		}
	}
	latch.texturesSent = 0;
	API_DestroyTexture( texture );
}
