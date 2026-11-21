// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	A level of the bloom chain from the one before (the first from the scene's viewport): a texel from the 4 x 4
	source texels around its 2 x 2, in 13 bilinear taps (Jimenez):

	  a . b . c      the centre box j k l m weighs 1/2, the four corner boxes (a b d e, b c e f, d e g h, e f h i)
	  . j . k .      1/8 each; the taps stay within the source's sourceMin .. sourceMax texel centres
	  d . e . f
	  . l . m .      karis: each box also weighs 1 / ( 1 + its luma ), so a lone bright pixel does not flicker
	  g . h . i      from frame to frame as the view moves (the first level only)
*/

#include "../include/exposure.hlsli"

cbuffer DownsampleParms : register( b0 ) {
	uint sourceSrv;
	uint targetUav;			// RGBA16F
	uint linearSampler;
	uint karis;
	float2 sourceOrigin;	// source texels: where the target's texel 0, 0 begins
	float2 invSourceSize;	// 1 / the size of the source view
	float2 sourceMin;
	float2 sourceMax;
	uint2 targetSize;
};

static const float BLOOM_MAX = 64000.0;	// below half's largest; an infinity would spread over the whole chain

float3 Tap( Texture2D<float4> source, SamplerState filter, float2 centre, float2 offset ) {
	const float2 p = clamp( centre + offset, sourceMin, sourceMax );
	return source.SampleLevel( filter, p * invSourceSize, 0 ).rgb;
}

float3 Box( float3 a, float3 b, float3 c, float3 d, float weight, inout float total ) {
	const float3 mean = ( a + b + c + d ) * 0.25;
	const float w = weight / ( 1.0 + dot( mean, EXPOSURE_LUMA ) );
	total += w;
	return mean * w;
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= targetSize ) ) {
		return;
	}
	Texture2D<float4> source = ResourceDescriptorHeap[sourceSrv];
	SamplerState filter = SamplerDescriptorHeap[linearSampler];
	const float2 centre = sourceOrigin + ( float2( id.xy ) + 0.5 ) * 2.0;
	const float3 a = Tap( source, filter, centre, float2( -2.0, -2.0 ) );
	const float3 b = Tap( source, filter, centre, float2( 0.0, -2.0 ) );
	const float3 c = Tap( source, filter, centre, float2( 2.0, -2.0 ) );
	const float3 d = Tap( source, filter, centre, float2( -2.0, 0.0 ) );
	const float3 e = Tap( source, filter, centre, float2( 0.0, 0.0 ) );
	const float3 f = Tap( source, filter, centre, float2( 2.0, 0.0 ) );
	const float3 g = Tap( source, filter, centre, float2( -2.0, 2.0 ) );
	const float3 h = Tap( source, filter, centre, float2( 0.0, 2.0 ) );
	const float3 i = Tap( source, filter, centre, float2( 2.0, 2.0 ) );
	const float3 j = Tap( source, filter, centre, float2( -1.0, -1.0 ) );
	const float3 k = Tap( source, filter, centre, float2( 1.0, -1.0 ) );
	const float3 l = Tap( source, filter, centre, float2( -1.0, 1.0 ) );
	const float3 m = Tap( source, filter, centre, float2( 1.0, 1.0 ) );
	float3 result;
	if ( karis != 0 ) {
		float total = 0.0;
		result = Box( j, k, l, m, 0.5, total ) + Box( a, b, d, e, 0.125, total ) + Box( b, c, e, f, 0.125, total )
			+ Box( d, e, g, h, 0.125, total ) + Box( e, f, h, i, 0.125, total );
		result /= total;
	} else {
		result = e * 0.125 + ( a + c + g + i ) * 0.03125 + ( b + d + f + h ) * 0.0625 + ( j + k + l + m ) * 0.125;
	}
	RWTexture2D<float4> target = ResourceDescriptorHeap[targetUav];
	target[id.xy] = float4( clamp( result, 0.0, BLOOM_MAX ), 1.0 );		// max( NaN, 0 ) is 0
}
