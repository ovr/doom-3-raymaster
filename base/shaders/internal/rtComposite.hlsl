// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The reflections (R20) into the HDR scene's viewport: what a pixel's rays met, as internal/rtReflect.hlsl or the
	filters after it leave it, times the surface's part: Schlick's Fresnel about the G-buffer's normal and the fade
	toward the roughest that reflects (include/rtReflection.hlsli), times the scale and preExposure. The blend adds
	it, or puts it in place of the colour (r_rtDebug 5); the write mask keeps the scene's alpha. A pixel whose
	G-buffer reflects nothing adds nothing, whatever the filters kept there. MULTISAMPLED reads sample 0.
*/

#include "../include/srgb.hlsli"
#include "../include/gbuffer.hlsli"
#include "../include/rtView.hlsli"
#include "../include/rtReflection.hlsli"

cbuffer CompositeParms : register( b1 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 origin;				// xyz the eye
	float4 viewport;			// x, y, width, height in the scene
	float4 limits;				// x the roughness that reflects nothing, y the fade below it, z the scale times preExposure
	int2 first;					// the reflection's first pixel in the scene
	uint reflectionSrv;
	uint depthSrv;
	uint normalSrv;				// GB0
	uint materialSrv;			// GB1
	uint unused0;
	uint unused1;
};

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	RT_SCENE_TEXTURE( float4 ) materialMap = ResourceDescriptorHeap[materialSrv];
	const int2 pixel = int2( position.xy );
	const float4 material = RT_SCENE_LOAD( materialMap, pixel );
	RtView eye;
	eye.clipToWorld = clipToWorld;
	eye.eye = origin.xyz;
	eye.viewport = viewport;
	eye.depthSrv = depthSrv;
	const float depth = RtDepthAt( eye, pixel );
	const float weight = RtReflectWeight( material.a, limits.xy );
	if ( depth >= 1.0 || weight <= 0.0 || !any( material.rgb > 0.0 ) ) {
		return float4( 0.0, 0.0, 0.0, 0.0 );
	}
	Texture2D<float4> reflection = ResourceDescriptorHeap[reflectionSrv];
	RT_SCENE_TEXTURE( float2 ) normalMap = ResourceDescriptorHeap[normalSrv];
	const float3 normal = DecodeOctahedral( RT_SCENE_LOAD( normalMap, pixel ) );
	const float3 view = normalize( RtPositionAt( eye, pixel, depth ) - origin.xyz );
	const float3 fresnel = RtReflectFresnel( material.rgb, material.a, dot( -view, normal ) );
	const float3 c = reflection.Load( int3( pixel - first, 0 ) ).rgb * fresnel * ( weight * limits.z );
	return float4( min( max( c, 0.0 ), HALF_MAX ), 0.0 );
}
