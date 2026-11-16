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

/*
====================
idRenderBackendGL::API_Cull

This handles the flipping needed when the view being
rendered is a mirored view.
After SetViewMirror the previous type is CULL_UNSET, and GL_CULL_FACE is not turned back on
if a two-sided pass turned it off: frames depend on that.
====================
*/
void idRenderBackendGL::API_Cull( int cullType, int previousCullType ) {
	if ( cullType == CT_TWO_SIDED ) {
		qglDisable( GL_CULL_FACE );
	} else  {
		if ( previousCullType == CT_TWO_SIDED ) {
			qglEnable( GL_CULL_FACE );
		}

		if ( cullType == CT_BACK_SIDED ) {
			if ( latch.viewIsMirror ) {
				qglCullFace( GL_FRONT );
			} else {
				qglCullFace( GL_BACK );
			}
		} else {
			if ( latch.viewIsMirror ) {
				qglCullFace( GL_BACK );
			} else {
				qglCullFace( GL_FRONT );
			}
		}
	}
}

/*
====================
idRenderBackendGL::API_BeginView
====================
*/
void idRenderBackendGL::API_BeginView() {
	// bind one global Vertex Array Object (VAO)
	qglBindVertexArray( glContext.global_vao );
}

/*
====================
idRenderBackendGL::API_Scissor
====================
*/
void idRenderBackendGL::API_Scissor( const renderRect_t & rect ) {
	qglScissor( rect.x, rect.y, rect.width, rect.height );
}

/*
====================
idRenderBackendGL::API_Viewport
====================
*/
void idRenderBackendGL::API_Viewport( const renderRect_t & rect ) {
	qglViewport( rect.x, rect.y, rect.width, rect.height );
}

/*
====================
idRenderBackendGL::API_PolygonOffset
====================
*/
void idRenderBackendGL::API_PolygonOffset( float scale, float bias ) {
	qglPolygonOffset( scale, bias );
}

/*
========================
idRenderBackendGL::API_DepthBoundsTest
========================
*/
void idRenderBackendGL::API_DepthBoundsTest( float zmin, float zmax ) {
	if ( zmin == 0.0f && zmax == 0.0f ) {
		qglDisable( GL_DEPTH_BOUNDS_TEST_EXT );
	} else {
		qglEnable( GL_DEPTH_BOUNDS_TEST_EXT );
		qglDepthBoundsEXT( zmin, zmax );
	}
}

/*
========================
idRenderBackendGL::API_Clear

GL clears inside its own scissor and through its own write masks: the rectangle and the mask are not read.
========================
*/
void idRenderBackendGL::API_Clear( const clearDesc_t & clear ) {
	int clearFlags = 0;
	if ( clear.color ) {
		qglClearColor( clear.rgba[0], clear.rgba[1], clear.rgba[2], clear.rgba[3] );
		clearFlags |= GL_COLOR_BUFFER_BIT;
	}
	if ( clear.depth ) {
		clearFlags |= GL_DEPTH_BUFFER_BIT;
	}
	if ( clear.stencil ) {
		qglClearStencil( clear.stencilValue );
		clearFlags |= GL_STENCIL_BUFFER_BIT;
	}
	qglClear( clearFlags );
}

/*
========================
idRenderBackendGL::API_SetDefaultState

This should initialize all GL state that any part of the entire program
may touch, including the editor.
========================
*/
void idRenderBackendGL::API_SetDefaultState() {
	RENDERLOG_PRINTF( "--- GL_SetDefaultState ---\n" );

	qglClearDepth( 1.0f );

	// make sure our GL state vector is set correctly
	// the latch is already reset
	currentVertexLayout = LAYOUT_UNKNOWN;
	currentVertexBuffer = bufferHandle_t::NONE;
	currentIndexBuffer = bufferHandle_t::NONE;
#ifdef _DEBUG
	debugTexturesBound = 0;
#endif
	API_State( 0, ~0ull );

	// These are changed by GL_Cull
	qglCullFace( GL_FRONT_AND_BACK );
	qglEnable( GL_CULL_FACE );

	// These are changed by GL_State
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglBlendFunc( GL_ONE, GL_ZERO );
	qglDepthMask( GL_TRUE );
	qglDepthFunc( GL_LESS );
	qglDisable( GL_STENCIL_TEST );
	qglDisable( GL_POLYGON_OFFSET_FILL );
	qglDisable( GL_POLYGON_OFFSET_LINE );
	qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

	// These should never be changed
	qglShadeModel( GL_SMOOTH );
	qglEnable( GL_DEPTH_TEST );
	qglEnable( GL_BLEND );
	qglEnable( GL_SCISSOR_TEST );
	qglDrawBuffer( GL_BACK );
	qglReadBuffer( GL_BACK );

	if ( r_useScissor.GetBool() ) {
		qglScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
	}
}

/*
====================
idRenderBackendGL::API_State

This routine is responsible for setting the most commonly changed state
diff is every bit with r_useStateCaching 0 and from API_SetDefaultState: everything is set
all the time then, so we can see if our delta checking is screwing up
====================
*/
void idRenderBackendGL::API_State( uint64 stateBits, uint64 diff ) {
	// One-sided KEEP, KEEP, INCR over a latched shadow preset used to issue nothing and keep the two-sided
	// ops; now it would replace them. Only the shadow pass sends these ops, and it carries the preset on.
	assert( diff == ~0ull || !GLS_IsStencilShadowOps( stateBits ^ diff )
		|| ( stateBits & GLS_STENCIL_OP_BITS ) != ( GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_INCR ) );

	//
	// check depthFunc bits
	//
	if ( diff & GLS_DEPTHFUNC_BITS ) {
		switch ( stateBits & GLS_DEPTHFUNC_BITS ) {
			case GLS_DEPTHFUNC_EQUAL:	qglDepthFunc( GL_EQUAL ); break;
			case GLS_DEPTHFUNC_ALWAYS:	qglDepthFunc( GL_ALWAYS ); break;
			case GLS_DEPTHFUNC_LESS:	qglDepthFunc( GL_LEQUAL ); break;
			case GLS_DEPTHFUNC_GREATER:	qglDepthFunc( GL_GEQUAL ); break;
		}
	}

	//
	// check blend bits
	//
	if ( diff & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) {
		GLenum srcFactor = GL_ONE;
		GLenum dstFactor = GL_ZERO;

		switch ( stateBits & GLS_SRCBLEND_BITS ) {
			case GLS_SRCBLEND_ZERO:					srcFactor = GL_ZERO; break;
			case GLS_SRCBLEND_ONE:					srcFactor = GL_ONE; break;
			case GLS_SRCBLEND_DST_COLOR:			srcFactor = GL_DST_COLOR; break;
			case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	srcFactor = GL_ONE_MINUS_DST_COLOR; break;
			case GLS_SRCBLEND_SRC_ALPHA:			srcFactor = GL_SRC_ALPHA; break;
			case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	srcFactor = GL_ONE_MINUS_SRC_ALPHA; break;
			case GLS_SRCBLEND_DST_ALPHA:			srcFactor = GL_DST_ALPHA; break;
			case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	srcFactor = GL_ONE_MINUS_DST_ALPHA; break;
			default:
				assert( !"GL_State: invalid src blend state bits\n" );
				break;
		}

		switch ( stateBits & GLS_DSTBLEND_BITS ) {
			case GLS_DSTBLEND_ZERO:					dstFactor = GL_ZERO; break;
			case GLS_DSTBLEND_ONE:					dstFactor = GL_ONE; break;
			case GLS_DSTBLEND_SRC_COLOR:			dstFactor = GL_SRC_COLOR; break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	dstFactor = GL_ONE_MINUS_SRC_COLOR; break;
			case GLS_DSTBLEND_SRC_ALPHA:			dstFactor = GL_SRC_ALPHA; break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	dstFactor = GL_ONE_MINUS_SRC_ALPHA; break;
			case GLS_DSTBLEND_DST_ALPHA:			dstFactor = GL_DST_ALPHA; break;
			case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:  dstFactor = GL_ONE_MINUS_DST_ALPHA; break;
			default:
				assert( !"GL_State: invalid dst blend state bits\n" );
				break;
		}

		// Only actually update GL's blend func if blending is enabled.
		if ( srcFactor == GL_ONE && dstFactor == GL_ZERO ) {
			qglDisable( GL_BLEND );
		} else {
			qglEnable( GL_BLEND );
			qglBlendFunc( srcFactor, dstFactor );
		}
	}

	//
	// check depthmask
	//
	if ( diff & GLS_DEPTHMASK ) {
		if ( stateBits & GLS_DEPTHMASK ) {
			qglDepthMask( GL_FALSE );
		} else {
			qglDepthMask( GL_TRUE );
		}
	}

	//
	// check colormask
	//
	if ( diff & (GLS_REDMASK|GLS_GREENMASK|GLS_BLUEMASK|GLS_ALPHAMASK) ) {
		GLboolean r = ( stateBits & GLS_REDMASK ) ? GL_FALSE : GL_TRUE;
		GLboolean g = ( stateBits & GLS_GREENMASK ) ? GL_FALSE : GL_TRUE;
		GLboolean b = ( stateBits & GLS_BLUEMASK ) ? GL_FALSE : GL_TRUE;
		GLboolean a = ( stateBits & GLS_ALPHAMASK ) ? GL_FALSE : GL_TRUE;
		qglColorMask( r, g, b, a );
	}

	//
	// fill/line mode
	//
	if ( diff & GLS_POLYMODE_LINE ) {
		if ( stateBits & GLS_POLYMODE_LINE ) {
			qglPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		} else {
			qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		}
	}

	//
	// polygon offset
	//
	if ( diff & GLS_POLYGON_OFFSET ) {
		if ( stateBits & GLS_POLYGON_OFFSET ) {
			qglPolygonOffset( latch.polyOfsScale, latch.polyOfsBias );
			qglEnable( GL_POLYGON_OFFSET_FILL );
			qglEnable( GL_POLYGON_OFFSET_LINE );
		} else {
			qglDisable( GL_POLYGON_OFFSET_FILL );
			qglDisable( GL_POLYGON_OFFSET_LINE );
		}
	}

	//
	// stencil
	//
	// the test is on when the function or the front ops are set; the back ops alone never turn it on
	if ( diff & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_FRONT_OP_BITS ) ) {
		if ( ( stateBits & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_FRONT_OP_BITS ) ) != 0 ) {
			qglEnable( GL_STENCIL_TEST );
		} else {
			qglDisable( GL_STENCIL_TEST );
		}
	}
	if ( diff & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_FUNC_REF_BITS | GLS_STENCIL_FUNC_MASK_BITS ) ) {
		GLuint ref = GLuint( ( stateBits & GLS_STENCIL_FUNC_REF_BITS ) >> GLS_STENCIL_FUNC_REF_SHIFT );
		GLuint mask = GLuint( ( stateBits & GLS_STENCIL_FUNC_MASK_BITS ) >> GLS_STENCIL_FUNC_MASK_SHIFT );
		GLenum func = 0;

		switch ( stateBits & GLS_STENCIL_FUNC_BITS ) {
			case GLS_STENCIL_FUNC_NEVER:		func = GL_NEVER; break;
			case GLS_STENCIL_FUNC_LESS:			func = GL_LESS; break;
			case GLS_STENCIL_FUNC_EQUAL:		func = GL_EQUAL; break;
			case GLS_STENCIL_FUNC_LEQUAL:		func = GL_LEQUAL; break;
			case GLS_STENCIL_FUNC_GREATER:		func = GL_GREATER; break;
			case GLS_STENCIL_FUNC_NOTEQUAL:		func = GL_NOTEQUAL; break;
			case GLS_STENCIL_FUNC_GEQUAL:		func = GL_GEQUAL; break;
			case GLS_STENCIL_FUNC_ALWAYS:		func = GL_ALWAYS; break;
		}
		qglStencilFunc( func, ref, mask );
	}
	if ( diff & GLS_STENCIL_OP_BITS ) {
		// the ops of one face, from bits in the front-face position
		const auto setStencilOps = []( GLenum face, uint64 ops ) {
			GLenum sFail = 0;
			GLenum zFail = 0;
			GLenum pass = 0;

			switch ( ops & GLS_STENCIL_OP_FAIL_BITS ) {
				case GLS_STENCIL_OP_FAIL_KEEP:		sFail = GL_KEEP; break;
				case GLS_STENCIL_OP_FAIL_ZERO:		sFail = GL_ZERO; break;
				case GLS_STENCIL_OP_FAIL_REPLACE:	sFail = GL_REPLACE; break;
				case GLS_STENCIL_OP_FAIL_INCR:		sFail = GL_INCR; break;
				case GLS_STENCIL_OP_FAIL_DECR:		sFail = GL_DECR; break;
				case GLS_STENCIL_OP_FAIL_INVERT:	sFail = GL_INVERT; break;
				case GLS_STENCIL_OP_FAIL_INCR_WRAP: sFail = GL_INCR_WRAP; break;
				case GLS_STENCIL_OP_FAIL_DECR_WRAP: sFail = GL_DECR_WRAP; break;
			}
			switch ( ops & GLS_STENCIL_OP_ZFAIL_BITS ) {
				case GLS_STENCIL_OP_ZFAIL_KEEP:		zFail = GL_KEEP; break;
				case GLS_STENCIL_OP_ZFAIL_ZERO:		zFail = GL_ZERO; break;
				case GLS_STENCIL_OP_ZFAIL_REPLACE:	zFail = GL_REPLACE; break;
				case GLS_STENCIL_OP_ZFAIL_INCR:		zFail = GL_INCR; break;
				case GLS_STENCIL_OP_ZFAIL_DECR:		zFail = GL_DECR; break;
				case GLS_STENCIL_OP_ZFAIL_INVERT:	zFail = GL_INVERT; break;
				case GLS_STENCIL_OP_ZFAIL_INCR_WRAP:zFail = GL_INCR_WRAP; break;
				case GLS_STENCIL_OP_ZFAIL_DECR_WRAP:zFail = GL_DECR_WRAP; break;
			}
			switch ( ops & GLS_STENCIL_OP_PASS_BITS ) {
				case GLS_STENCIL_OP_PASS_KEEP:		pass = GL_KEEP; break;
				case GLS_STENCIL_OP_PASS_ZERO:		pass = GL_ZERO; break;
				case GLS_STENCIL_OP_PASS_REPLACE:	pass = GL_REPLACE; break;
				case GLS_STENCIL_OP_PASS_INCR:		pass = GL_INCR; break;
				case GLS_STENCIL_OP_PASS_DECR:		pass = GL_DECR; break;
				case GLS_STENCIL_OP_PASS_INVERT:	pass = GL_INVERT; break;
				case GLS_STENCIL_OP_PASS_INCR_WRAP:	pass = GL_INCR_WRAP; break;
				case GLS_STENCIL_OP_PASS_DECR_WRAP:	pass = GL_DECR_WRAP; break;
			}
			if ( face == GL_FRONT_AND_BACK ) {
				qglStencilOp( sFail, zFail, pass );
			} else {
				qglStencilOpSeparate( face, sFail, zFail, pass );
			}
		};
		if ( stateBits & GLS_SEPARATE_STENCIL ) {
			setStencilOps( GL_FRONT, stateBits );
			setStencilOps( GL_BACK, stateBits >> GLS_STENCIL_BACK_OP_SHIFT );
		} else {
			setStencilOps( GL_FRONT_AND_BACK, stateBits );
		}
	}
}
