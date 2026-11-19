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
#include "qgl.h"

#include "../tr_local.h"
#include "gl_Backend.h"

/*
================================================================================================

	Draw: the vertex layouts and idRenderBackendGL::API_DrawIndexed

================================================================================================
*/

idCVar r_glCheckLatch( "r_glCheckLatch", "0", CVAR_RENDERER | CVAR_BOOL, "compare GL's face culling and depth function with the latch rules on every draw" );

// the indexes of a draw are triIndex_t
static const GLenum GL_INDEX_TYPE = GL_UNSIGNED_SHORT;
static_assert( sizeof( triIndex_t ) == sizeof( GLushort ) );

// every input a layout can have, in the order the arrays are switched on and off
static const int vertexAttribIndexes[] = {
	PC_ATTRIB_INDEX_VERTEX, PC_ATTRIB_INDEX_NORMAL, PC_ATTRIB_INDEX_COLOR,
	PC_ATTRIB_INDEX_COLOR2, PC_ATTRIB_INDEX_ST, PC_ATTRIB_INDEX_TANGENT
};

struct glAttribFormat_t {
	GLint		size;
	GLenum		type;
	GLboolean	normalized;
};

// indexed by vertexAttribFormat_t
static const glAttribFormat_t glAttribFormats[] = {
	{ 3, GL_FLOAT, GL_FALSE },			// FLOAT3
	{ 4, GL_FLOAT, GL_FALSE },			// FLOAT4
	{ 2, GL_HALF_FLOAT, GL_TRUE },		// HALF2
	{ 4, GL_UNSIGNED_BYTE, GL_TRUE }	// UBYTE4_NORM
};

/*
========================
GL_SetVertexLayout

Describes the vertex buffer GL has bound to GL_ARRAY_BUFFER.
========================
*/
static void GL_SetVertexLayout( const vertexLayoutDesc_t & layout ) {
	for ( const int index : vertexAttribIndexes ) {
		if ( ( layout.attribMask & ( 1 << index ) ) != 0 ) {
			qglEnableVertexAttribArrayARB( index );
		} else {
			qglDisableVertexAttribArrayARB( index );
		}
	}

	for ( int i = 0; i < layout.numAttribs; i++ ) {
		const vertexAttribDesc_t & attrib = layout.attribs[i];
		const glAttribFormat_t & format = glAttribFormats[static_cast<int>( attrib.format )];
		qglVertexAttribPointerARB( attrib.index, format.size, format.type, format.normalized, layout.stride, reinterpret_cast<void *>( static_cast<uintptr_t>( attrib.offset ) ) );
	}
}

/*
========================
idRenderBackendGL::API_DrawIndexed

The order is the one a surface always had: joints, parms, index buffer, vertex buffer and layout, draw.
========================
*/
void idRenderBackendGL::API_DrawIndexed( const drawCall_t & dc ) {
#ifdef _DEBUG
	DebugCheckDraw( dc );
#endif
	if ( r_glCheckLatch.GetBool() ) {
		CheckLatch();
	}

	if ( dc.jointBuffer != bufferHandle_t::NONE ) {
		qglBindBufferRange( GL_UNIFORM_BUFFER, 0, GL_Buffer( dc.jointBuffer )->bufferObject, dc.jointOffset, dc.numJoints * sizeof( idJointMat ) );
	}

	// without a program nothing was sent before either
	if ( !dc.reuseParms && latch.program != programHandle_t::NONE ) {
		CommitParms( latch.program, dc.parms );
	}

	if ( currentIndexBuffer != dc.indexBuffer || !r_useStateCaching.GetBool() ) {
		qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, GL_Buffer( dc.indexBuffer )->bufferObject );
		currentIndexBuffer = dc.indexBuffer;
	}

	if ( ( currentVertexLayout != dc.layout ) || ( currentVertexBuffer != dc.vertexBuffer ) || !r_useStateCaching.GetBool() ) {
		qglBindBufferARB( GL_ARRAY_BUFFER_ARB, GL_Buffer( dc.vertexBuffer )->bufferObject );
		currentVertexBuffer = dc.vertexBuffer;

		GL_SetVertexLayout( vertexLayouts[dc.layout] );
		currentVertexLayout = dc.layout;
	}

	qglDrawElementsBaseVertex( ( dc.topology == primitive_t::LINES ) ? GL_LINES : GL_TRIANGLES,
							  dc.numIndexes,
							  GL_INDEX_TYPE,
							  reinterpret_cast<triIndex_t *>( static_cast<uintptr_t>( dc.indexOffset ) ),
							  dc.baseVertex );
}

// What a pipeline backend builds from latch.cullEnabled and latch.depthFuncStrict, against GL itself.
void idRenderBackendGL::CheckLatch() {
	GLenum depthFunc = GL_LESS;
	if ( !latch.depthFuncStrict ) {
		switch ( latch.stateBits & GLS_DEPTHFUNC_BITS ) {
			case GLS_DEPTHFUNC_EQUAL:	depthFunc = GL_EQUAL; break;
			case GLS_DEPTHFUNC_ALWAYS:	depthFunc = GL_ALWAYS; break;
			case GLS_DEPTHFUNC_GREATER:	depthFunc = GL_GEQUAL; break;
			default:					depthFunc = GL_LEQUAL; break;
		}
	}
	const GLenum cullMode = ( ( latch.cull == CT_BACK_SIDED ) != latch.viewIsMirror ) ? GL_BACK : GL_FRONT;
	const bool oneSided = latch.cullEnabled && latch.cull != CT_TWO_SIDED;

	GLint gotDepthFunc = 0;
	GLint gotCullMode = 0;
	qglGetIntegerv( GL_DEPTH_FUNC, &gotDepthFunc );
	qglGetIntegerv( GL_CULL_FACE_MODE, &gotCullMode );
	const bool gotCullEnabled = ( qglIsEnabled( GL_CULL_FACE ) == GL_TRUE );

	latchChecks++;
	if ( gotCullEnabled == latch.cullEnabled && static_cast<GLenum>( gotDepthFunc ) == depthFunc
			&& ( !oneSided || static_cast<GLenum>( gotCullMode ) == cullMode ) ) {
		return;
	}
	if ( latchMismatches++ < MAX_LATCH_WARNINGS ) {
		common->Warning( "r_glCheckLatch: mismatch at draw %d: GL_CULL_FACE %d, latch %d; GL_DEPTH_FUNC 0x%x, latch 0x%x; GL_CULL_FACE_MODE 0x%x, latch 0x%x (cull %d, mirror %d)",
			latchChecks, gotCullEnabled ? 1 : 0, latch.cullEnabled ? 1 : 0, gotDepthFunc, depthFunc,
			gotCullMode, oneSided ? cullMode : 0, latch.cull, latch.viewIsMirror ? 1 : 0 );
	}
}

// Shutdown and vid_restart print what r_glCheckLatch found since the last report.
void idRenderBackendGL::ReportLatchChecks() {
	if ( latchChecks > 0 ) {
		common->Printf( "r_glCheckLatch: %d draws checked, %d mismatches\n", latchChecks, latchMismatches );
	}
	latchChecks = 0;
	latchMismatches = 0;
}

#ifdef _DEBUG

/*
========================
idRenderBackendGL::DebugCheckDraw

What OpenGL forgives and a pipeline backend does not. No OpenGL call in here; every finding
is printed once per program.
========================
*/
void idRenderBackendGL::DebugCheckDraw( const drawCall_t & dc ) {
	assert( IsOwnerThread() );

	glProgram_t * prog = GL_Program( latch.program );
	if ( prog == nullptr ) {
		return;
	}
	const programInfo_t & info = prog->info;

	// rule R11: every slot the program samples was bound in this frame, with the kind the program reads
	for ( int slot = 0; slot < MAX_TEXTURE_SLOTS; slot++ ) {
		const uint16 slotBit = static_cast<uint16>( 1 << slot );
		if ( ( info.samplerMask & slotBit ) == 0 ) {
			continue;
		}
		const int kind = ( ( info.cubeMask & slotBit ) != 0 ) ? 1 : 0;
		glTexture_t * tex = GL_Texture( latch.textures[slot][kind] );
		if ( tex != nullptr && ( debugTexturesBound & ( 1u << ( slot * 2 + kind ) ) ) != 0 ) {
			tex->drawnInFrame = debugFrame;
		} else if ( ( prog->warnedSamplers & slotBit ) == 0 ) {
			prog->warnedSamplers |= slotBit;
			common->Printf( "idRenderBackendGL::DrawIndexed: program '%s' samples %s slot %d, which this frame has not bound\n",
				prog->name.c_str(), ( kind != 0 ) ? "cube" : "2D", slot );
		}
	}

	// the inputs of the vertex stage come from the layout
	const uint16 missingAttribs = static_cast<uint16>( info.attribMask & ~vertexLayouts[dc.layout].attribMask );
	if ( missingAttribs != 0 && ( prog->warnedLayouts & ( 1 << dc.layout ) ) == 0 ) {
		prog->warnedLayouts |= static_cast<uint8>( 1 << dc.layout );
		common->Printf( "idRenderBackendGL::DrawIndexed: program '%s' reads vertex inputs 0x%x, which layout %d does not have\n",
			prog->name.c_str(), missingAttribs, dc.layout );
	}

	// joints
	if ( dc.jointBuffer != bufferHandle_t::NONE && !prog->warnedJoints ) {
		if ( ( dc.jointOffset % caps.uniformBufferOffsetAlignment ) != 0 ) {
			prog->warnedJoints = true;
			common->Printf( "idRenderBackendGL::DrawIndexed: joint offset %d of a draw with program '%s' is not a multiple of %d\n",
				dc.jointOffset, prog->name.c_str(), caps.uniformBufferOffsetAlignment );
		} else if ( !info.usesJoints ) {
			prog->warnedJoints = true;
			common->Printf( "idRenderBackendGL::DrawIndexed: a skinned draw with program '%s', which reads no joints\n", prog->name.c_str() );
		}
	}

	// rule R7: reuseParms promises the program and the packed parms of the draw before
	idVec4 packed[2][MAX_RENDER_PARMS];
	const int numParms[2] = {
		R_PackRenderParms( packed[0], dc.parms, prog->vertexInfo ),
		R_PackRenderParms( packed[1], dc.parms, prog->fragmentInfo )
	};
	if ( dc.reuseParms ) {
		bool same = ( debugLastProgram == latch.program );
		for ( int stage = 0; stage < 2 && same; stage++ ) {
			same = ( numParms[stage] == debugLastNumParms[stage] )
				&& memcmp( packed[stage], debugLastParms[stage], numParms[stage] * sizeof( idVec4 ) ) == 0;
		}
		if ( !same && debugParmsReuseBroken++ == 0 ) {
			common->Printf( "idRenderBackendGL::DrawIndexed: reuseParms with program '%s', but the program or its parms changed since the last draw\n", prog->name.c_str() );
		}
	} else {
		debugLastProgram = latch.program;
		for ( int stage = 0; stage < 2; stage++ ) {
			debugLastNumParms[stage] = numParms[stage];
			memcpy( debugLastParms[stage], packed[stage], numParms[stage] * sizeof( idVec4 ) );
		}
	}
}

#endif	// _DEBUG
