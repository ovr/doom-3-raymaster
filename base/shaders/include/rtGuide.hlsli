// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The guides of the indirect light's grid (R19 of RenderBackend.h), which internal/rtGuides.hlsl writes a texel
	per stride pixels a side, each its block's first pixel:

	  guides     R32_UINT  bits 0-6 and 7-13 the normal of the plane the depth holds (octahedral), 14-29 the clip w
	                       at the depth (log2 from 1/4 up, 4096 steps an octave), 31 a surface; 0 where nothing is drawn
	  slope      RG16F     the change of 1 / clip w per texel along x and y, over 1 / clip w

	internal/rtUpsample.hlsl takes a texel for a pixel only where that is the pixel's surface: on the plane its depth
	and slope give (1 / clip w is linear over a plane on the screen).
*/

#include "gbuffer.hlsli"

static const uint RT_GUIDE_DRAWN = 0x80000000u;
static const float RT_GUIDE_MIN_LOG = -2.0;
static const float RT_GUIDE_OCTAVES = 16.0;

struct RtGuide {
	float3	normal;
	float	w;			// clip w
	bool	drawn;
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
	return guide;
}

// 1 for a neighbour, offset texels away, on the plane of the texel; less the farther it lies from it.
float RtPlaneWeight( float w, float2 slope, float2 offset, float otherW ) {
	const float expected = 1.0 + dot( slope, offset );
	const float tolerance = 0.004 + 0.25 * ( abs( slope.x * offset.x ) + abs( slope.y * offset.y ) );
	return exp( -abs( w / otherW - expected ) / tolerance );
}
