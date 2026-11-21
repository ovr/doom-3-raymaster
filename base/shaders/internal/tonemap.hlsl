// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The tonemap of the HDR resolve: the scene colour (radiance times preExposure)
// times exposure / preExposure, through the Combine LUT by its log coordinate, dithered by half a step of
// the 8-bit output from a hash of the pixel, so two runs give the same bytes.

#include "../include/srgb.hlsli"

cbuffer TonemapParms : register( b1 ) {
	uint sceneSrv;			// RGBA16F, one sample: the scene or its MSAA resolve
	uint lutSrv;			// Texture3D 32^3
	uint lutSampler;		// linear, clamp
	uint dither;
	float exposureScale;	// exposure / preExposure
	float lutK;				// the encoding of d3d12_Hdr.h: t = log2( c / k + 1 ) / L
	float lutInvRange;		// 1 / L
	float unused;
};

// Jimenez's interleaved gradient noise: 0..1, no frame index.
float Noise( float2 pixel ) {
	return frac( 52.9829189 * frac( dot( pixel, float2( 0.06711056, 0.00583715 ) ) ) );
}

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	Texture2D<float4> scene = ResourceDescriptorHeap[sceneSrv];
	Texture3D<float4> lut = ResourceDescriptorHeap[lutSrv];
	SamplerState lutFilter = SamplerDescriptorHeap[lutSampler];
	const float4 sceneColor = scene.Load( int3( position.xy, 0 ) );
	const float3 c = clamp( sceneColor.rgb * exposureScale, 0.0, HALF_MAX );	// no NaN or infinity reaches the LUT
	const float3 t = saturate( log2( c / lutK + 1.0 ) * lutInvRange );
	float3 color = lut.SampleLevel( lutFilter, t * ( 31.0 / 32.0 ) + 0.5 / 32.0, 0 ).rgb;
	if ( dither != 0 ) {
		color += ( Noise( position.xy ) - 0.5 ) / 255.0;
	}
	return float4( saturate( color ), saturate( sceneColor.a ) );
}
