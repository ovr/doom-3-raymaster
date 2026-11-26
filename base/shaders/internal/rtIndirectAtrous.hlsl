// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	One pass of the spatial filter of the indirect light (R19), after internal/rtIndirectTemporal.hlsl: a 5 x 5 B3
	spline whose taps lie stride texels apart. A tap counts as far as it is the texel's surface (the plane of its
	depth, the plane's facing: include/rtDenoise.hlsli) and as its luminance is within what the texel's variance
	explains, or within a fiftieth of it: a light that has settled keeps its steps. A texel of fewer than
	RT_CUT_FRAMES frames takes every tap of its surface: a cut or a disoccluded strip is blurred, not speckled.

	  amount  in, out  include/rtBounce.hlsli's
	  aux     in, out  r, g the mean Co and Cg, b the variance of the mean luminance, which each pass lowers, a the
	                   frames; 0: the texel holds nothing, and no texel takes it
*/

#include "../include/rtDenoise.hlsli"

cbuffer IndirectAtrousParms : register( b0 ) {
	uint2 size;				// texels
	uint amountSrv;
	uint auxSrv;
	uint guidesSrv;
	uint slopeSrv;
	uint amountUav;
	uint auxUav;
	int stride;				// texels between two taps
	uint unused0;
	uint unused1;
	uint unused2;
};

static const float SPLINE[3] = { 0.375, 0.25, 0.0625 };
static const float GAUSS[3] = { 0.25, 0.125, 0.0625 };	// 3 x 3, by |x| + |y|
static const float LUMINANCE_RANGE = 4.0;	// standard deviations a tap's luminance may lie off the texel's
static const float LUMINANCE_PART = 0.02;	// and this part of the texel's luminance

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	Texture2D<float4> amountMap = ResourceDescriptorHeap[amountSrv];
	Texture2D<float4> auxMap = ResourceDescriptorHeap[auxSrv];
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	RWTexture2D<float4> amountOut = ResourceDescriptorHeap[amountUav];
	RWTexture2D<float4> auxOut = ResourceDescriptorHeap[auxUav];

	const int2 texel = int2( dispatch.xy );
	const float4 centre = amountMap.Load( int3( texel, 0 ) );
	const float4 centreAux = auxMap.Load( int3( texel, 0 ) );
	const uint packed = guides.Load( int3( texel, 0 ) );
	if ( ( packed & RT_GUIDE_DRAWN ) == 0 || centreAux.a < 0.5 ) {
		amountOut[dispatch.xy] = centre;
		auxOut[dispatch.xy] = centreAux;
		return;
	}
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	const RtGuide guide = RtUnpackGuide( packed );
	const float2 slope = slopes.Load( int3( texel, 0 ) );

	// one texel's variance is a rough guess: its 3 x 3 instead
	float variance = 0.0;
	float total = 0.0;
	for ( int v = -1; v <= 1; v++ ) {
		for ( int u = -1; u <= 1; u++ ) {
			const int2 q = texel + int2( u, v );
			if ( all( q >= 0 ) && all( q < int2( size ) ) ) {
				const float4 around = auxMap.Load( int3( q, 0 ) );
				if ( around.a >= 0.5 ) {
					const float g = GAUSS[abs( u ) + abs( v )];
					variance += g * around.b;
					total += g;
				}
			}
		}
	}
	// A texel of fewer frames knows no variance, and its neighbours' spread keeps a lone bright ray a dot: every tap
	// of its surface counts.
	const float range = ( centreAux.a < RT_CUT_FRAMES ) ? 1e30 : LUMINANCE_RANGE * sqrt( max( variance / total, 0.0 ) ) + LUMINANCE_PART * centre.a + 1e-6;

	// the taps as differences to the centre: a texel among its equals keeps its value to the bit
	float weight = SPLINE[0] * SPLINE[0];
	float4 sum = float4( 0.0, 0.0, 0.0, 0.0 );
	float2 sumChroma = float2( 0.0, 0.0 );
	float sumVariance = centreAux.b * weight * weight;
	for ( int y = -2; y <= 2; y++ ) {
		for ( int x = -2; x <= 2; x++ ) {
			const int2 q = texel + int2( x, y ) * stride;
			if ( ( x != 0 || y != 0 ) && all( q >= 0 ) && all( q < int2( size ) ) ) {
				const uint otherPacked = guides.Load( int3( q, 0 ) );
				const float4 tapAux = auxMap.Load( int3( q, 0 ) );
				if ( ( otherPacked & RT_GUIDE_DRAWN ) != 0 && tapAux.a >= 0.5 ) {
					const float4 tap = amountMap.Load( int3( q, 0 ) );
					const RtGuide other = RtUnpackGuide( otherPacked );
					const float facing = saturate( dot( guide.normal, other.normal ) );
					const float facing2 = facing * facing;
					const float facing4 = facing2 * facing2;
					const float w = SPLINE[abs( x )] * SPLINE[abs( y )]
						* RtPlaneWeight( guide.w, slope, float2( x, y ) * float( stride ), other.w )
						* facing4 * facing4
						* exp( -abs( centre.a - tap.a ) / range );
					sum += ( tap - centre ) * w;
					sumChroma += ( tapAux.rg - centreAux.rg ) * w;
					sumVariance += tapAux.b * w * w;
					weight += w;
				}
			}
		}
	}
	const float4 mixed = centre + sum / weight;
	amountOut[dispatch.xy] = float4( RtHalf( mixed.rgb ), RtHalf( mixed.a ) );
	auxOut[dispatch.xy] = float4( RtHalf( float3( centreAux.rg + sumChroma / weight, 0.0 ) ).xy, sumVariance / ( weight * weight ), centreAux.a );
}
