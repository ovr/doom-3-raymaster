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
