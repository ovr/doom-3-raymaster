// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	NRD's guides of the main view (R18 with r_rtDenoiser 1), a texel per pixel of the viewport, rows from the top
	as NRD's clip space (D3D's) has them; the scene's count up from the bottom:

	  IN_VIEWZ             R32F         the clip w of the point the depth puts there, the weapon's a quarter of it
	                                    (its depth hack); NRD_SKY_VIEWZ where nothing is drawn
	  IN_NORMAL_ROUGHNESS  R10G10B10A2  NRD_FrontEnd_PackNormalAndRoughness of the G-buffer's bumped normal and
	                                    roughness; material 1 where the pixel's reflection is glossy (R20), else 0
	  IN_MV                RGBA16F      2.5D motion: xy to the frame before in NRD's uv, z its clip w then minus now,
	                                    for a still world (0 for the weapon, which moves with the eye)

	MULTISAMPLED reads sample 0.
*/

#include "../include/gbuffer.hlsli"
#include "../include/rtView.hlsli"
#include "../include/rtReflection.hlsli"
#include "../include/nrd/NRD.hlsli"

cbuffer GuidesParms : register( b0 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 clipW;				// row 3 of the MVP: the clip w of a world point
	float4 prevClipW;			// of the frame before's
	float4 origin;				// xyz the eye
	float4 viewport;			// x, y, width, height in the scene
	// x the roughness that reflects nothing, y the fade below it, z below this roughness a mirror, w the sky's view z
	float4 limits;
	int2 first;					// the viewport's first pixel in the scene
	uint2 size;					// texels
	uint depthSrv;
	uint normalSrv;				// GB0
	uint materialSrv;			// GB1
	uint motionSrv;				// GB2
	uint albedoSrv;				// GB3
	uint viewZUav;
	uint normalRoughnessUav;
	uint motionUav;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	RWTexture2D<float> viewZOut = ResourceDescriptorHeap[viewZUav];
	RWTexture2D<float4> normalRoughnessOut = ResourceDescriptorHeap[normalRoughnessUav];
	RWTexture2D<float4> motionOut = ResourceDescriptorHeap[motionUav];
	RT_SCENE_TEXTURE( float4 ) albedoMap = ResourceDescriptorHeap[albedoSrv];
	const int2 pixel = first + int2( id.x, size.y - 1 - id.y );
	const float drawn = RT_SCENE_LOAD( albedoMap, pixel ).a;

	RtView eye;
	eye.clipToWorld = clipToWorld;
	eye.eye = origin.xyz;
	eye.viewport = viewport;
	eye.depthSrv = depthSrv;
	const float depth = RtDepthAt( eye, pixel );
	if ( !GBufferDrawn( drawn ) || depth >= 1.0 ) {
		viewZOut[id.xy] = limits.w;
		normalRoughnessOut[id.xy] = NRD_FrontEnd_PackNormalAndRoughness( float3( 0.0, 0.0, 1.0 ), 1.0, 0.0 );
		motionOut[id.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	RT_SCENE_TEXTURE( float2 ) normalMap = ResourceDescriptorHeap[normalSrv];
	RT_SCENE_TEXTURE( float4 ) materialMap = ResourceDescriptorHeap[materialSrv];
	RT_SCENE_TEXTURE( float2 ) motionMap = ResourceDescriptorHeap[motionSrv];
	const float3 position = RtPositionAt( eye, pixel, depth );
	const bool weapon = GBufferWeapon( drawn );
	// the depth hack: a quarter of the clip z is four times the distance, along the same line
	const float scale = weapon ? 0.25 : 1.0;
	const float w = dot( clipW, float4( position, 1.0 ) ) * scale;
	const float prevW = dot( prevClipW, float4( position, 1.0 ) );

	const float4 material = RT_SCENE_LOAD( materialMap, pixel );
	const float roughness = material.a;
	const bool glossy = RtReflectWeight( roughness, limits.xy ) > 0.0 && any( material.rgb > 0.0 ) && roughness >= limits.z;
	const float3 normal = DecodeOctahedral( RT_SCENE_LOAD( normalMap, pixel ) );
	// the motion is this frame's position minus the frame before's, in pixels, +y up
	const float2 motion = RT_SCENE_LOAD( motionMap, pixel );

	viewZOut[id.xy] = w;
	normalRoughnessOut[id.xy] = NRD_FrontEnd_PackNormalAndRoughness( normal, roughness, glossy ? 1.0 : 0.0 );
	motionOut[id.xy] = float4( -motion.x / viewport.z, motion.y / viewport.w, weapon ? 0.0 : prevW - w, 0.0 );
}
