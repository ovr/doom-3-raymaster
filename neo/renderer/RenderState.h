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

#ifndef __RENDERSTATE_H__
#define __RENDERSTATE_H__

// one/zero is flipped on src/dest so a gl state of 0 is SRC_ONE,DST_ZERO
static const uint64 GLS_SRCBLEND_ONE					= 0 << 0;
static const uint64 GLS_SRCBLEND_ZERO					= 1 << 0;
static const uint64 GLS_SRCBLEND_DST_COLOR				= 2 << 0;
static const uint64 GLS_SRCBLEND_ONE_MINUS_DST_COLOR	= 3 << 0;
static const uint64 GLS_SRCBLEND_SRC_ALPHA				= 4 << 0;
static const uint64 GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA	= 5 << 0;
static const uint64 GLS_SRCBLEND_DST_ALPHA				= 6 << 0;
static const uint64 GLS_SRCBLEND_ONE_MINUS_DST_ALPHA	= 7 << 0;
static const uint64 GLS_SRCBLEND_BITS					= 7 << 0;

static const uint64 GLS_DSTBLEND_ZERO					= 0 << 3;
static const uint64 GLS_DSTBLEND_ONE					= 1 << 3;
static const uint64 GLS_DSTBLEND_SRC_COLOR				= 2 << 3;
static const uint64 GLS_DSTBLEND_ONE_MINUS_SRC_COLOR	= 3 << 3;
static const uint64 GLS_DSTBLEND_SRC_ALPHA				= 4 << 3;
static const uint64 GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA	= 5 << 3;
static const uint64 GLS_DSTBLEND_DST_ALPHA				= 6 << 3;
static const uint64 GLS_DSTBLEND_ONE_MINUS_DST_ALPHA	= 7 << 3;
static const uint64 GLS_DSTBLEND_BITS					= 7 << 3;

//------------------------
// these masks are the inverse, meaning when set the glColorMask value will be 0,
// preventing that channel from being written
//------------------------
static const uint64 GLS_DEPTHMASK						= 1 << 6;
static const uint64 GLS_REDMASK							= 1 << 7;
static const uint64 GLS_GREENMASK						= 1 << 8;
static const uint64 GLS_BLUEMASK						= 1 << 9;
static const uint64 GLS_ALPHAMASK						= 1 << 10;
static const uint64 GLS_COLORMASK						= (GLS_REDMASK|GLS_GREENMASK|GLS_BLUEMASK);

static const uint64 GLS_POLYMODE_LINE					= 1 << 11;
static const uint64 GLS_POLYGON_OFFSET					= 1 << 12;

static const uint64 GLS_DEPTHFUNC_LESS					= 0 << 13;
static const uint64 GLS_DEPTHFUNC_ALWAYS				= 1 << 13;
static const uint64 GLS_DEPTHFUNC_GREATER				= 2 << 13;
static const uint64 GLS_DEPTHFUNC_EQUAL					= 3 << 13;
static const uint64 GLS_DEPTHFUNC_BITS					= 3 << 13;

// stencil bits
static const uint64 GLS_STENCIL_FUNC_REF_SHIFT			= 20;
static const uint64 GLS_STENCIL_FUNC_REF_BITS			= 0xFFll << GLS_STENCIL_FUNC_REF_SHIFT;

static const uint64 GLS_STENCIL_FUNC_MASK_SHIFT			= 28;
static const uint64 GLS_STENCIL_FUNC_MASK_BITS			= 0xFFll << GLS_STENCIL_FUNC_MASK_SHIFT;

#define GLS_STENCIL_MAKE_REF( x ) ( ( (uint64)(x) << GLS_STENCIL_FUNC_REF_SHIFT ) & GLS_STENCIL_FUNC_REF_BITS )
#define GLS_STENCIL_MAKE_MASK( x ) ( ( (uint64)(x) << GLS_STENCIL_FUNC_MASK_SHIFT ) & GLS_STENCIL_FUNC_MASK_BITS )

static const uint64 GLS_STENCIL_FUNC_ALWAYS				= 0ull << 36;
static const uint64 GLS_STENCIL_FUNC_LESS				= 1ull << 36;
static const uint64 GLS_STENCIL_FUNC_LEQUAL				= 2ull << 36;
static const uint64 GLS_STENCIL_FUNC_GREATER			= 3ull << 36;
static const uint64 GLS_STENCIL_FUNC_GEQUAL				= 4ull << 36;
static const uint64 GLS_STENCIL_FUNC_EQUAL				= 5ull << 36;
static const uint64 GLS_STENCIL_FUNC_NOTEQUAL			= 6ull << 36;
static const uint64 GLS_STENCIL_FUNC_NEVER				= 7ull << 36;
static const uint64 GLS_STENCIL_FUNC_BITS				= 7ull << 36;

static const uint64 GLS_STENCIL_OP_FAIL_KEEP			= 0ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_ZERO			= 1ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_REPLACE			= 2ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_INCR			= 3ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_DECR			= 4ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_INVERT			= 5ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_INCR_WRAP		= 6ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_DECR_WRAP		= 7ull << 39;
static const uint64 GLS_STENCIL_OP_FAIL_BITS			= 7ull << 39;

static const uint64 GLS_STENCIL_OP_ZFAIL_KEEP			= 0ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_ZERO			= 1ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_REPLACE		= 2ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_INCR			= 3ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_DECR			= 4ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_INVERT			= 5ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_INCR_WRAP		= 6ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_DECR_WRAP		= 7ull << 42;
static const uint64 GLS_STENCIL_OP_ZFAIL_BITS			= 7ull << 42;

static const uint64 GLS_STENCIL_OP_PASS_KEEP			= 0ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_ZERO			= 1ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_REPLACE			= 2ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_INCR			= 3ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_DECR			= 4ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_INVERT			= 5ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_INCR_WRAP		= 6ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_DECR_WRAP		= 7ull << 45;
static const uint64 GLS_STENCIL_OP_PASS_BITS			= 7ull << 45;

// back-face stencil ops, read only when GLS_SEPARATE_STENCIL is set; clear = both faces use the front ops
static const uint64 GLS_STENCIL_FRONT_OP_BITS			= GLS_STENCIL_OP_FAIL_BITS | GLS_STENCIL_OP_ZFAIL_BITS | GLS_STENCIL_OP_PASS_BITS;	// bits 39-47
static const uint64 GLS_STENCIL_BACK_OP_SHIFT			= 9;				// front ops << 9 = the same ops for the back face, bits 48-56
static const uint64 GLS_STENCIL_BACK_OP_BITS			= GLS_STENCIL_FRONT_OP_BITS << GLS_STENCIL_BACK_OP_SHIFT;
static const uint64 GLS_SEPARATE_STENCIL				= 1ull << 57;
static const uint64 GLS_STENCIL_OP_BITS					= GLS_STENCIL_FRONT_OP_BITS | GLS_STENCIL_BACK_OP_BITS | GLS_SEPARATE_STENCIL;

constexpr uint64 GLS_BackStencilOps( uint64 frontOps ) { return ( frontOps & GLS_STENCIL_FRONT_OP_BITS ) << GLS_STENCIL_BACK_OP_SHIFT; }

// stencil shadow volumes, both faces in one draw
static const uint64 GLS_STENCIL_SHADOW_ZPASS			= GLS_SEPARATE_STENCIL
	| GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_INCR
	| GLS_BackStencilOps( GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_DECR );
static const uint64 GLS_STENCIL_SHADOW_PRELOAD			= GLS_SEPARATE_STENCIL
	| GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_DECR | GLS_STENCIL_OP_PASS_DECR
	| GLS_BackStencilOps( GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_INCR | GLS_STENCIL_OP_PASS_INCR );

// the light volume of the stencil select pass (r_useLightStencilSelect), both faces in one draw
static const uint64 GLS_STENCIL_LIGHT_SELECT			= GLS_SEPARATE_STENCIL
	| GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_REPLACE | GLS_STENCIL_OP_PASS_ZERO
	| GLS_BackStencilOps( GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_ZERO | GLS_STENCIL_OP_PASS_REPLACE );

// true when the stencil ops of a state are one of the two shadow volume presets
constexpr bool GLS_IsStencilShadowOps( uint64 stateBits ) {
	return ( stateBits & GLS_STENCIL_OP_BITS ) == GLS_STENCIL_SHADOW_ZPASS || ( stateBits & GLS_STENCIL_OP_BITS ) == GLS_STENCIL_SHADOW_PRELOAD;
}

static const uint64 GLS_OVERRIDE						= 1ull << 63;		// override the render prog state

// what a pipeline object depends on: everything but the dynamic stencil reference and the front-end flag
static const uint64 GLS_PIPELINE_BITS					= ~( GLS_STENCIL_FUNC_REF_BITS | GLS_OVERRIDE );

static const uint64 GLS_DEFAULT = 0;

// How texture_color_soft fades a particle where it meets the scene behind it (rpSoftParticle.y), so that a fade of 0
// leaves the target as it was: by the stage's blend. NONE: a blend that cannot fade.
enum class softParticleMode_t : int { NONE = -1, COLOR = 0, ALPHA = 1, PREMULTIPLIED = 2, FILTER = 3 };

constexpr softParticleMode_t GLS_SoftParticleMode( uint64 stateBits ) {
	const uint64 src = stateBits & GLS_SRCBLEND_BITS;
	const uint64 dst = stateBits & GLS_DSTBLEND_BITS;
	if ( dst == GLS_DSTBLEND_ONE || ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) ) {
		return softParticleMode_t::COLOR;			// an add, or a darkening one minus src
	}
	if ( src == GLS_SRCBLEND_SRC_ALPHA && dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) {
		return softParticleMode_t::ALPHA;
	}
	if ( src == GLS_SRCBLEND_ONE && dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) {
		return softParticleMode_t::PREMULTIPLIED;
	}
	if ( ( src == GLS_SRCBLEND_DST_COLOR && dst == GLS_DSTBLEND_ZERO ) || ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_SRC_COLOR ) ) {
		return softParticleMode_t::FILTER;			// a multiply: the colour goes toward white
	}
	return softParticleMode_t::NONE;
}

#define STENCIL_SHADOW_TEST_VALUE		128
#define STENCIL_SHADOW_MASK_VALUE		255

#endif /* !__RENDERSTATE_H__ */
