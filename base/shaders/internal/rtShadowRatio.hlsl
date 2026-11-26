// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	What the traced shadows leave of the traced lights (R16), per pixel and channel: 1 - the light target over the
	scene, which holds those lights' sum without shadows. Under MSAA over the pixel's samples. Kept as the part
	left, not the part taken: a half float is finest near 0, where a dim light stands in a bright one's shadow.
	A channel no light reaches gets what the others keep (its product is 0 anyway): the picture of r_rtDebug 6
	and a filter read it. Alpha is the luminance of the sum, 0 where no traced light reaches.
*/

#include "../include/srgb.hlsli"

cbuffer RatioParms : register( b1 ) {
	int2 first;				// the viewport's first pixel in the scene
	uint sceneSrv;			// RGBA16F, the scene's samples
	uint lightsSrv;			// the same of the light target
	uint samples;
};

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	const int2 pixel = int2( position.xy ) + first;
#if MULTISAMPLED
	Texture2DMS<float4> scene = ResourceDescriptorHeap[sceneSrv];
	Texture2DMS<float4> lights = ResourceDescriptorHeap[lightsSrv];
	float3 sum = 0.0;
	float3 taken = 0.0;
	for ( uint i = 0; i < samples; i++ ) {
		sum += max( scene.Load( pixel, i ).rgb, 0.0 );
		taken += max( lights.Load( pixel, i ).rgb, 0.0 );
	}
	sum /= float( samples );
	taken /= float( samples );
#else
	Texture2D<float4> scene = ResourceDescriptorHeap[sceneSrv];
	Texture2D<float4> lights = ResourceDescriptorHeap[lightsSrv];
	const float3 sum = max( scene.Load( int3( pixel, 0 ) ).rgb, 0.0 );
	const float3 taken = max( lights.Load( int3( pixel, 0 ) ).rgb, 0.0 );
#endif
	// the difference first: all of a light taken is 0 to the bit, whatever the division makes of x / x
	const float3 weights = float3( 0.2126, 0.7152, 0.0722 );
	const float3 kept = max( sum - taken, 0.0 );
	const float luminance = dot( sum, weights );
	const float all = ( luminance > 0.0 ) ? saturate( dot( kept, weights ) / luminance ) : 1.0;
	const float3 left = select( sum > 0.0, saturate( kept / max( sum, 1e-20 ) ), all );
	return float4( left, min( luminance, HALF_MAX ) );
}
