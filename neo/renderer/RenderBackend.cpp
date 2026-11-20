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

#include <bit>

#include "tr_local.h"

idRenderBackend *	renderBackend = nullptr;

idCVar r_renderApi( "r_renderApi", "d3d12", CVAR_RENDERER | CVAR_INIT | CVAR_ARCHIVE, "graphics API of the renderer, read once at startup" );

idRenderBackendFactory * idRenderBackendFactory::staticFactories = nullptr;

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
	latch.cullEnabled = true;
	latch.depthFuncStrict = true;
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

// idDrawStreamCheck hashes with FNV-1a over 32-bit words what decides a draw's pixels, without handles
// and buffer offsets: parallel front end jobs take vertex cache space with atomic adds, so the offsets
// of a frame change from run to run.
namespace {

class idDrawHash {
public:
	explicit idDrawHash( uint64 seed ) : hash( seed ) {}
	void		Word( uint32 word ) { hash = ( hash ^ word ) * 0x100000001b3ull; }
	void		Int( int value ) { Word( static_cast<uint32>( value ) ); }
	void		Float( float value ) { Word( std::bit_cast<uint32>( value ) ); }
	void		Rect( const renderRect_t & rect ) { Int( rect.x ); Int( rect.y ); Int( rect.width ); Int( rect.height ); }
	uint64		Value() const { return hash; }
private:
	uint64		hash;
};

}	// namespace

void idDrawStreamCheck::BeginFrame() {
	maxDraws = r_debugMaxDraws.GetInteger();
	hashing = r_hashDrawStream.GetBool();
	active = hashing || maxDraws >= 0;
	numDraws = 0;
	hash = 0xcbf29ce484222325ull;
	numFrames++;
}

void idDrawStreamCheck::EndFrame() {
	if ( hashing ) {
		common->Printf( "r_hashDrawStream: frame %d, %d draws, %016llx\n", numFrames, numDraws, static_cast<unsigned long long>( hash ) );
	}
	active = false;
}

bool idDrawStreamCheck::Admit( const renderLatch_t & latch, const drawCall_t & dc ) {
	numDraws++;
	if ( hashing ) {
		idDrawHash h( hash );
		h.Word( static_cast<uint32>( latch.stateBits ) );
		h.Word( static_cast<uint32>( latch.stateBits >> 32 ) );
		h.Int( latch.cull );
		h.Int( ( latch.viewIsMirror ? 1 : 0 ) | ( latch.cullEnabled ? 2 : 0 ) | ( latch.depthFuncStrict ? 4 : 0 ) );
		h.Float( latch.polyOfsScale );
		h.Float( latch.polyOfsBias );
		h.Float( latch.depthBoundsMin );
		h.Float( latch.depthBoundsMax );
		h.Rect( latch.viewport );
		h.Rect( latch.scissor );
		uint32 boundSlots = 0;
		for ( int slot = 0; slot < MAX_TEXTURE_SLOTS; slot++ ) {
			for ( int kind = 0; kind < 2; kind++ ) {
				if ( latch.textures[slot][kind] != textureHandle_t::NONE ) {
					boundSlots |= 1u << ( slot * 2 + kind );
				}
			}
		}
		h.Word( boundSlots );
		h.Int( dc.layout | ( static_cast<int>( dc.topology ) << 8 ) | ( dc.reuseParms ? 1 << 16 : 0 ) | ( dc.jointBuffer != bufferHandle_t::NONE ? 1 << 17 : 0 ) );
		h.Int( dc.numIndexes );
		h.Int( dc.numJoints );
		if ( dc.parms != nullptr ) {
			for ( int i = 0; i < MAX_RENDER_PARMS; i++ ) {
				for ( int j = 0; j < 4; j++ ) {
					h.Float( dc.parms[i][j] );
				}
			}
		}
		hash = h.Value();
	}
	return maxDraws < 0 || numDraws <= maxDraws;
}
