// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	What the filters of the ray traced passes share (R18 of RenderBackend.h). internal/rtGuides.hlsl writes, a texel
	per pixel of the viewport:

	  guides     R32_UINT  bits 0-6 and 7-13 the G-buffer's world normal (octahedral), 14-29 the clip w at the depth
	                       (log2 from 1/4 up, 4096 steps an octave), 30 a hole that keeps the frame before's,
	                       31 a surface; 0 where nothing is drawn. The next frame tests its histories against it.
	  reproject  RGBA16F   xy the G-buffer's motion in pixels, z the clip w the point had the frame before over
	                       this frame's
	  slope      RG16F     the change of 1 / clip w per pixel along x and y, over 1 / clip w

	A filter takes a neighbour, and a history a texel of the frame before, only where that is the pixel's surface:
	on the plane its depth and slope give (1 / clip w is linear over a plane on the screen), turned the same way.
*/

#include "gbuffer.hlsli"

static const float3 RT_LUMA = float3( 0.2126, 0.7152, 0.0722 );

static const uint RT_GUIDE_DRAWN = 0x80000000u;
static const uint RT_GUIDE_BRIDGED = 0x40000000u;
static const float RT_GUIDE_MIN_LOG = -2.0;
static const float RT_GUIDE_OCTAVES = 16.0;

struct RtGuide {
	float3	normal;
	float	w;			// clip w
	bool	drawn;
	bool	bridged;	// nothing was drawn here this frame: the frame before's guide, once
};

uint RtPackGuide( float2 octahedral, float clipW ) {
	const uint2 n = uint2( round( saturate( octahedral * 0.5 + 0.5 ) * 127.0 ) );
	const float t = saturate( ( log2( max( clipW, 1e-6 ) ) - RT_GUIDE_MIN_LOG ) / RT_GUIDE_OCTAVES );
	return RT_GUIDE_DRAWN | ( uint( round( t * 65535.0 ) ) << 14 ) | ( n.y << 7 ) | n.x;
}

RtGuide RtUnpackGuide( uint packed ) {
	RtGuide guide;
	guide.normal = DecodeOctahedral( float2( packed & 127u, ( packed >> 7 ) & 127u ) / 127.0 * 2.0 - 1.0 );
	guide.w = exp2( float( ( packed >> 14 ) & 65535u ) / 65535.0 * RT_GUIDE_OCTAVES + RT_GUIDE_MIN_LOG );
	guide.drawn = ( packed & RT_GUIDE_DRAWN ) != 0;
	guide.bridged = ( packed & RT_GUIDE_BRIDGED ) != 0;
	return guide;
}

// 1 for a neighbour, offset pixels away, on the plane of the pixel; less the farther it lies from it.
float RtPlaneWeight( float w, float2 slope, float2 offset, float otherW ) {
	const float expected = 1.0 + dot( slope, offset );
	const float tolerance = 0.004 + 0.25 * ( abs( slope.x * offset.x ) + abs( slope.y * offset.y ) );
	return exp( -abs( w / otherW - expected ) / tolerance );
}

// A texel of the frame before: the pixel's surface when its depth is the one the pixel had then, within the part
// tolerance, and it is turned as the pixel is. The normals are the bumped ones, so the angle is wide.
bool RtSameSurface( RtGuide pixel, float expectedW, float tolerance, RtGuide other ) {
	return other.drawn && abs( other.w - expectedW ) <= tolerance * expectedW && dot( pixel.normal, other.normal ) > 0.6;
}

// The nearest value a half float holds. A store into one drops the bits below its last (toward zero, WARP and
// AMD alike): an average would lose half a step with every frame, a pass of a filter with every pass.
float3 RtHalf( float3 x ) {
	const int3 exponent = max( int3( ( asuint( x ) >> 23 ) & 255u ) - 127, -14 );
	const float3 step = asfloat( uint3( exponent + 117 ) << 23 );	// 2 ^ ( exponent - 10 )
	return round( x / step ) * step;
}

float RtHalf( float x ) {
	return RtHalf( float3( x, 0.0, 0.0 ) ).x;
}

// An average's step toward the frame's value. What keeps the average drops a step smaller than half of its last
// bit: the average of a constant would stay beside it for good, and two runs that differed once would never hold
// the same bits again. So a step is at least that bit (leastStep: the bit over the value, 1 / 1024 for a half).
float3 RtAverage( float3 mean, float3 value, float blend, float leastStep ) {
	const float3 gap = value - mean;
	const float3 least = min( abs( gap ), max( abs( mean ), 6.2e-5 ) * leastStep );
	return mean + sign( gap ) * max( abs( gap ) * blend, least );
}

float RtAverage( float mean, float value, float blend, float leastStep ) {
	return RtAverage( float3( mean, 0.0, 0.0 ), float3( value, 0.0, 0.0 ), blend, leastStep ).x;
}

static const float RT_CUT_FRAMES = 4.0;			// frames a history needs to tell its variance, a side, and to be cut
static const float RT_EDGE_STEP = 1.0 / 16.0;	// the least step between the two sides of an edge
static const float RT_EDGE_DEVIATIONS = 4.0;	// of a texel's noise its step must exceed
#define RT_TILE 8								// pixels a side of a tile of the gradient (internal/rtGradient.hlsl)

// The variance of one frame's luminance that an average's two moments tell. Half floats round the two apart by up
// to 5e-4 of the second: no noise yet.
float RtKnownNoise( float mean, float second ) {
	return max( second - mean * mean - 5e-4 * second, 0.0 );
}

struct RtHeld {
	float3	mean;
	float	frames;		// 0: the pixel has no history
	float	second;		// the average of the luminance squared
};

// What the history holds of a pixel: the four texels around where its centre was the frame before, each only of the
// pixel's surface. sides: a texel across an edge the history holds without noise is left out (rtTemporal.hlsl).
// None where the measure left the texels' range by a quarter: that history is of another signal.
RtHeld RtReadHistory( int2 pixel, uint2 size, RtGuide guide, float2 slope, float3 value, float measure, bool sides, Texture2D<float4> reproject,
		Texture2D<uint> prevGuides, Texture2D<float4> prevColor, Texture2D<float2> prevAux ) {
	RtHeld history;
	history.mean = float3( 0.0, 0.0, 0.0 );
	history.frames = 0.0;
	history.second = 0.0;
	const float3 moved = reproject.Load( int3( pixel, 0 ) ).xyz;
	// the scene counts its rows up, as the motion does: where the pixel's centre was, in texels
	const float2 at = float2( pixel ) - moved.xy;
	const float2 base = floor( at );
	const float2 part = at - base;
	const float expected = guide.w * moved.z;
	const float tolerance = 0.02 + abs( slope.x ) + abs( slope.y );
	float4 tapColor[4];
	float2 tapAux[4];
	float tapWeight[4];
	float tapFar[4];		// from the frame's value
	float nearest = 1.0;
	[unroll]
	for ( uint tap = 0; tap < 4; tap++ ) {
		const int2 offset = int2( tap & 1u, tap >> 1 );
		const float w = ( ( offset.x != 0 ) ? part.x : 1.0 - part.x ) * ( ( offset.y != 0 ) ? part.y : 1.0 - part.y );
		const int2 q = int2( base ) + offset;
		tapColor[tap] = float4( 0.0, 0.0, 0.0, 0.0 );
		tapAux[tap] = float2( 0.0, 0.0 );
		tapWeight[tap] = 0.0;
		tapFar[tap] = 0.0;
		if ( w > 0.0 && all( q >= 0 ) && all( q < int2( size ) ) ) {
			const RtGuide other = RtUnpackGuide( prevGuides.Load( int3( q, 0 ) ) );
			const float4 held = prevColor.Load( int3( q, 0 ) );
			// a texel without a frame held nothing: no light reached it
			if ( held.a >= 1.0 && RtSameSurface( guide, expected, tolerance, other ) ) {
				const float3 apart = abs( held.rgb - value );
				tapColor[tap] = held;
				tapAux[tap] = prevAux.Load( int3( q, 0 ) );
				tapWeight[tap] = w;
				tapFar[tap] = max( apart.r, max( apart.g, apart.b ) );
				nearest = min( nearest, tapFar[tap] );
			}
		}
	}
	float3 mean = float3( 0.0, 0.0, 0.0 );
	float frames = 0.0;
	float weight = 0.0;
	float least = 3.4e38;	// of the taps' measures
	float most = 0.0;
	// The taps as differences to the first one: equal ones give their value to the bit. Of the luminance it is
	// the variance that is mixed: the second moment of two texels that differ would count their difference as noise.
	float4 first = float4( 0.0, 0.0, 0.0, 0.0 );
	float firstNoise = 0.0;
	float noise = 0.0;
	[unroll]
	for ( uint slot = 0; slot < 4; slot++ ) {
		const float4 held = tapColor[slot];
		const float2 heldAux = tapAux[slot];
		// An edge the history holds without noise is not mixed: of its two sides the pixel takes its own. A
		// texel farther from the frame's value than RT_EDGE_DEVIATIONS of its noise explain, and much farther
		// than the nearest texel, lies across the edge. A penumbra's lone sample has no texel near it: all stay.
		const float heldLuminance = dot( held.rgb, RT_LUMA );
		const float heldNoise = heldAux.x - heldLuminance * heldLuminance;
		const float deviations = RT_EDGE_DEVIATIONS * RT_EDGE_DEVIATIONS * max( heldNoise - 5e-4 * heldAux.x, 0.0 );
		const bool sure = held.a >= RT_CUT_FRAMES && tapFar[slot] * tapFar[slot] > deviations;
		const bool across = sides && sure && tapFar[slot] > max( RT_EDGE_STEP, 2.0 * nearest );
		const float w = across ? 0.0 : tapWeight[slot];
		if ( w > 0.0 ) {
			if ( weight == 0.0 ) {
				first = held;
				firstNoise = heldNoise;
			}
			mean += w * ( held.rgb - first.rgb );
			frames += w * ( held.a - first.a );
			noise += w * ( heldNoise - firstNoise );
			least = min( least, heldAux.y );
			most = max( most, heldAux.y );
			weight += w;
		}
	}
	if ( weight > 0.0 && measure <= most * 1.25 + 1e-6 && measure >= least * 0.75 - 1e-6 ) {
		history.mean = first.rgb + mean / weight;
		history.frames = first.a + frames / weight;
		const float mixed = dot( history.mean, RT_LUMA );
		history.second = max( mixed * mixed + firstNoise + noise / weight, 0.0 );
	}
	return history;
}
