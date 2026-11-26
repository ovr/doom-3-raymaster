// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	One pass of the spatial filter of the glossy reflections (R20), after internal/rtReflectTemporal.hlsl: a 5 x 5 B3
	spline whose taps lie stride texels apart. A tap counts as far as it is the pixel's surface (the plane of its
	depth: include/rtDenoise.hlsli), turned and rough as the pixel is, no farther off than the pixel's lobe reaches
	on the screen (include/rtReflection.hlsli), and as its luminance is within what the pixel's variance explains.
	So a rough floor is blurred as wide as its lobe and no wider, and where a reflection touches what it reflects
	it stays sharp. A pixel of fewer than RT_CUT_FRAMES frames knows no variance and takes every such tap.

	  colour  in, out  rgb the mean, a the frames in it
	  spread  in, out  include/rtReflection.hlsli's; each pass lowers the variance. A texel that is no mean to filter
	                   passes as it is, and no texel takes it.
*/

#include "../include/rtDenoise.hlsli"
#include "../include/rtReflection.hlsli"

cbuffer ReflectAtrousParms : register( b0 ) {
	uint2 size;				// texels
	uint colorSrv;
	uint spreadSrv;
	uint guidesSrv;
	uint slopeSrv;
	uint colorUav;
	uint spreadUav;
	int stride;				// texels between two taps
	float pixelSpread;		// radians per pixel
	uint unused0;
	uint unused1;
};

static const float SPLINE[3] = { 0.375, 0.25, 0.0625 };
static const float GAUSS[3] = { 0.25, 0.125, 0.0625 };	// 3 x 3, by |x| + |y|
static const float LUMINANCE_RANGE = 4.0;	// standard deviations a tap's luminance may lie off the pixel's
static const float LUMINANCE_PART = 0.02;	// and this part of the pixel's luminance
static const float LEAST_LOBE = 0.35;		// pixels: a lobe narrower than this is filtered as this wide

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	Texture2D<float4> colorMap = ResourceDescriptorHeap[colorSrv];
	Texture2D<float4> spreadMap = ResourceDescriptorHeap[spreadSrv];
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	RWTexture2D<float4> colorOut = ResourceDescriptorHeap[colorUav];
	RWTexture2D<float4> spreadOut = ResourceDescriptorHeap[spreadUav];

	const int2 texel = int2( dispatch.xy );
	const float4 centre = colorMap.Load( int3( texel, 0 ) );
	const float4 centreSpread = spreadMap.Load( int3( texel, 0 ) );
	const uint packed = guides.Load( int3( texel, 0 ) );
	if ( ( packed & RT_GUIDE_DRAWN ) == 0 || centreSpread.a < RT_REFLECT_KIND_MEAN - 0.5 || centre.a < 0.5 ) {
		colorOut[dispatch.xy] = centre;
		spreadOut[dispatch.xy] = centreSpread;
		return;
	}
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	const RtGuide guide = RtUnpackGuide( packed );
	const float2 slope = slopes.Load( int3( texel, 0 ) );
	const float luminance = dot( centre.rgb, RT_LUMA );
	const float roughness = centreSpread.b;

	// one texel's variance is a rough guess: its 3 x 3 instead
	float variance = 0.0;
	float total = 0.0;
	for ( int v = -1; v <= 1; v++ ) {
		for ( int u = -1; u <= 1; u++ ) {
			const int2 q = texel + int2( u, v );
			if ( all( q >= 0 ) && all( q < int2( size ) ) ) {
				const float4 around = spreadMap.Load( int3( q, 0 ) );
				if ( around.a >= RT_REFLECT_KIND_MEAN - 0.5 ) {
					const float g = GAUSS[abs( u ) + abs( v )];
					variance += g * around.r;
					total += g;
				}
			}
		}
	}
	const float range = ( centre.a < RT_CUT_FRAMES ) ? 1e30 : LUMINANCE_RANGE * sqrt( max( variance / total, 0.0 ) ) + LUMINANCE_PART * luminance + 1e-6;
	const float lobe = max( RtReflectLobePixels( roughness, centreSpread.g, guide.w, pixelSpread ), LEAST_LOBE );
	const float perLobe = 0.5 / ( lobe * lobe );
	const float perRoughness = 1.0 / ( 0.1 * roughness + 0.01 );

	// the taps as differences to the centre: a texel among its equals keeps its value to the bit
	float weight = SPLINE[0] * SPLINE[0];
	float3 sum = float3( 0.0, 0.0, 0.0 );
	float sumVariance = centreSpread.r * weight * weight;
	for ( int y = -2; y <= 2; y++ ) {
		for ( int x = -2; x <= 2; x++ ) {
			const int2 q = texel + int2( x, y ) * stride;
			if ( ( x != 0 || y != 0 ) && all( q >= 0 ) && all( q < int2( size ) ) ) {
				const uint otherPacked = guides.Load( int3( q, 0 ) );
				const float4 tapSpread = spreadMap.Load( int3( q, 0 ) );
				const float4 tap = colorMap.Load( int3( q, 0 ) );
				if ( ( otherPacked & RT_GUIDE_DRAWN ) != 0 && tapSpread.a >= RT_REFLECT_KIND_MEAN - 0.5 && tap.a >= 0.5 ) {
					const RtGuide other = RtUnpackGuide( otherPacked );
					const float2 offset = float2( x, y ) * float( stride );
					const float w = SPLINE[abs( x )] * SPLINE[abs( y )]
						* RtPlaneWeight( guide.w, slope, offset, other.w )
						* RtReflectFacing( guide.normal, other.normal, roughness )
						* exp( -abs( roughness - tapSpread.b ) * perRoughness - dot( offset, offset ) * perLobe
							- abs( luminance - dot( tap.rgb, RT_LUMA ) ) / range );
					sum += ( tap.rgb - centre.rgb ) * w;
					sumVariance += tapSpread.r * w * w;
					weight += w;
				}
			}
		}
	}
	colorOut[dispatch.xy] = float4( RtHalf( centre.rgb + sum / weight ), centre.a );
	spreadOut[dispatch.xy] = float4( sumVariance / ( weight * weight ), centreSpread.gba );
}
