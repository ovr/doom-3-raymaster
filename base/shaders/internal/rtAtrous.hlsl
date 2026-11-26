// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	One pass of the spatial filter of a ray traced signal (R18), after internal/rtTemporal.hlsl: a 5 x 5 B3 spline
	whose taps lie stride pixels apart, so a few passes of growing stride reach far for the cost of 25 taps each.
	A tap counts as far as it is the pixel's surface (include/rtDenoise.hlsli: its plane, its facing) and as its
	luminance is within what the pixel's variance explains: where the history holds no noise, nothing is blurred.

	  input, output  rgb the signal, a the variance of its luminance, which each pass lowers as it averages;
	                 negative: the pixel is no part of the signal, and is passed on as it is
*/

#include "../include/rtDenoise.hlsli"

cbuffer AtrousParms : register( b0 ) {
	uint2 size;				// of the viewport
	uint inputSrv;
	uint guidesSrv;
	uint slopeSrv;
	uint outputUav;
	int stride;				// pixels between two taps
	uint unused;
};

static const float SPLINE[3] = { 0.375, 0.25, 0.0625 };
static const float GAUSS[3] = { 0.25, 0.125, 0.0625 };	// 3 x 3, by |x| + |y|
static const float LUMINANCE_RANGE = 4.0;	// standard deviations a tap's luminance may lie off the pixel's

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	Texture2D<float4> input = ResourceDescriptorHeap[inputSrv];
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];

	const int2 pixel = int2( dispatch.xy );
	const float4 centre = input.Load( int3( pixel, 0 ) );
	const uint packed = guides.Load( int3( pixel, 0 ) );
	// nothing drawn, or no part of the signal (the variance is negative): no pixel takes it either
	if ( ( packed & RT_GUIDE_DRAWN ) == 0 || centre.a < 0.0 ) {
		output[dispatch.xy] = centre;
		return;
	}
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	const RtGuide guide = RtUnpackGuide( packed );
	const float2 slope = slopes.Load( int3( pixel, 0 ) );

	// one pixel's variance is a rough guess: its 3 x 3 instead
	float variance = 0.0;
	float total = 0.0;
	for ( int v = -1; v <= 1; v++ ) {
		for ( int u = -1; u <= 1; u++ ) {
			const int2 q = pixel + int2( u, v );
			if ( all( q >= 0 ) && all( q < int2( size ) ) ) {
				const float around = input.Load( int3( q, 0 ) ).a;
				if ( around >= 0.0 ) {
					const float g = GAUSS[abs( u ) + abs( v )];
					variance += g * around;
					total += g;
				}
			}
		}
	}
	const float luminance = dot( centre.rgb, RT_LUMA );
	const float range = LUMINANCE_RANGE * sqrt( max( variance / total, 0.0 ) ) + 1e-5;

	// the taps as differences to the centre: a pixel among its equals keeps its value to the bit
	float weight = SPLINE[0] * SPLINE[0];
	float3 sum = float3( 0.0, 0.0, 0.0 );
	float sumVariance = centre.a * weight * weight;
	for ( int y = -2; y <= 2; y++ ) {
		for ( int x = -2; x <= 2; x++ ) {
			const int2 q = pixel + int2( x, y ) * stride;
			if ( ( x != 0 || y != 0 ) && all( q >= 0 ) && all( q < int2( size ) ) ) {
				const uint otherPacked = guides.Load( int3( q, 0 ) );
				const float4 tap = input.Load( int3( q, 0 ) );
				if ( ( otherPacked & RT_GUIDE_DRAWN ) != 0 && tap.a >= 0.0 ) {
					const RtGuide other = RtUnpackGuide( otherPacked );
					const float facing = saturate( dot( guide.normal, other.normal ) );
					const float facing2 = facing * facing;
					const float facing4 = facing2 * facing2;
					const float w = SPLINE[abs( x )] * SPLINE[abs( y )]
						* RtPlaneWeight( guide.w, slope, float2( x, y ) * float( stride ), other.w )
						* facing4 * facing4
						* exp( -abs( luminance - dot( tap.rgb, RT_LUMA ) ) / range );
					sum += ( tap.rgb - centre.rgb ) * w;
					sumVariance += tap.a * w * w;
					weight += w;
				}
			}
		}
	}
	output[dispatch.xy] = float4( RtHalf( centre.rgb + sum / weight ), sumVariance / ( weight * weight ) );
}
