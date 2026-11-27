// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	r_rtReflections (R20): a ray per pixel of the main view whose G-buffer may reflect (F0 above 0, roughness below
	the limit), from the point the depth puts there, against the GPU scene. Below the mirror's roughness it leaves
	about the G-buffer's normal and its hit is lit as the lights light it, each with a shadow ray toward its centre:
	the same every frame. Above it the ray leaves about a facet of the GGX lobe and its hit is lit by one of the
	lights that reach it, highlight and all, with a ray toward the light's disk (include/rtLighting.hlsli): noise
	the filters take.
	Written as include/rtReflection.hlsli names it; rows count up from the bottom as GL's, as the scene's do.

	Three pairs of numbers per glossy pixel and frame (include/rtNoise.hlsli, each with its own step): the facet,
	the light, the point of its disk. MULTISAMPLED reads sample 0.

	CHECKERBOARD (flags bit 0, under NRD): a thread per two pixels of a row. The one of NRD's checkerboard this frame
	(REBLUR's WHITE: x + its row from the top + the frame even) sends its ray, the other only if it is a mirror.
*/

#include "../include/gbuffer.hlsli"
#include "../include/rtLighting.hlsli"
#include "../include/rtNoise.hlsli"
#include "../include/rtView.hlsli"
#include "../include/rtReflection.hlsli"

cbuffer ReflectParms : register( b0 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 origin;				// xyz the eye, w the radians per pixel
	float4 viewport;			// x, y, width, height in the scene
	// x the longest ray, y the roughness that reflects nothing, z the fade below it, w below this roughness a mirror
	float4 limits;
	uint tlasSrv;
	uint recordsSrv;
	uint instancesSrv;
	uint verticesSrv;
	uint indexesSrv;
	uint skinnedSrv;
	uint lightsSrv;
	uint numLights;
	uint depthSrv;
	uint normalSrv;				// GB0
	uint materialSrv;			// GB1
	uint outputUav;
	uint linearSampler;
	uint clampSampler;
	uint maxLights;
	uint shadows;
	int2 first;					// the first pixel in the scene
	uint2 size;					// texels
	uint noiseSrv;
	uint noiseIndex;
	uint flags;					// bit 0: the checkerboard
	uint phase;					// the checkerboard's: a row from the bottom traces x of its parity plus this one's
};

// The widest cone a glossy ray reads its hit's textures with: a rough lobe averages them anyway.
static const float GLOSS_SPREAD = 0.1;

// The ray of the pixel at texel; a glossy one only with glossyHere, nothing reflected without it.
float4 Reflect( uint2 texel, bool glossyHere ) {
	RT_SCENE_TEXTURE( float4 ) materialMap = ResourceDescriptorHeap[materialSrv];
	const int2 pixel = first + int2( texel );
	const float4 material = RT_SCENE_LOAD( materialMap, pixel );
	RtView eye;
	eye.clipToWorld = clipToWorld;
	eye.eye = origin.xyz;
	eye.viewport = viewport;
	eye.depthSrv = depthSrv;
	const float depth = RtDepthAt( eye, pixel );
	const float roughness = material.a;

	float4 reflected = float4( 0.0, 0.0, 0.0, RT_REFLECT_NONE );
	if ( depth < 1.0 && RtReflectWeight( roughness, limits.yz ) > 0.0 && any( material.rgb > 0.0 ) && ( glossyHere || roughness < limits.w ) ) {
		RT_SCENE_TEXTURE( float2 ) normalMap = ResourceDescriptorHeap[normalSrv];
		const float3 position = RtPositionAt( eye, pixel, depth );
		const float3 normal = DecodeOctahedral( RT_SCENE_LOAD( normalMap, pixel ) );
		const float3 plane = RtDepthNormal( eye, pixel, depth, position );
		const float eyeDistance = length( position - origin.xyz );
		const float3 view = ( position - origin.xyz ) / max( eyeDistance, 1e-6 );
		const bool mirror = roughness < limits.w;

		float3 facet = normal;
		float spread = origin.w;
		if ( !mirror ) {
			// in the frame of the normal; an eye behind a bump's horizon sees it from just above
			const float3 other = ( abs( normal.z ) < 0.9 ) ? float3( 0.0, 0.0, 1.0 ) : float3( 1.0, 0.0, 0.0 );
			const float3 right = normalize( cross( other, normal ) );
			const float3 up = cross( normal, right );
			const float3 toEye = normalize( float3( -dot( view, right ), -dot( view, up ), max( -dot( view, normal ), 0.05 ) ) );
			const float alpha = roughness * roughness;
			const float3 local = RtGgxVisibleNormal( toEye, alpha, RtNoise( noiseSrv, texel + uint2( 19, 41 ), noiseIndex ) );
			facet = normalize( right * local.x + up * local.y + normal * local.z );
			spread = max( origin.w, min( alpha, GLOSS_SPREAD ) );
		}
		// a bumped normal or a facet may turn the ray into the surface: it stays above the plane the depth holds
		float3 direction = reflect( view, facet );
		const float above = dot( direction, plane );
		if ( above < 0.01 ) {
			direction = normalize( direction + plane * ( 0.01 - above ) );
		}

		RtHeap heap;
		heap.tlas = tlasSrv;
		heap.records = recordsSrv;
		heap.instances = instancesSrv;
		heap.vertices = verticesSrv;
		heap.indexes = indexesSrv;
		heap.skinned = skinnedSrv;
		RtLighting lighting;
		lighting.lights = lightsSrv;
		lighting.numLights = numLights;
		lighting.maxLights = maxLights;
		lighting.shadows = shadows != 0;
		lighting.linearSampler = linearSampler;
		lighting.clampSampler = clampSampler;

		// off the surface by more than the depth's error there
		RayDesc ray;
		ray.Origin = position + plane * ( 0.02 + 0.001 * eyeDistance );
		ray.Direction = direction;
		ray.TMin = 0.0;
		ray.TMax = limits.x;
		const RtHit hit = RtTrace( heap, ray, RT_MASK_VISIBLE, spread, linearSampler );
		float3 radiance = float3( 0.0, 0.0, 0.0 );
		if ( hit.hit && mirror ) {
			radiance = RtShade( heap, lighting, hit, ray, eyeDistance, origin.w );
		} else if ( hit.hit ) {
			// the pixel's cone has come to the surface, then widens as the lobe
			const float pick = RtNoise( noiseSrv, texel + uint2( 59, 7 ), noiseIndex, 1 ).x;
			const float2 spot = RtDisk( RtNoise( noiseSrv, texel + uint2( 23, 61 ), noiseIndex, 2 ) );
			radiance = RtShadeSampled( heap, lighting, hit, ray, eyeDistance * origin.w / spread, spread, pick, spot, 1.0, true );
		}
		reflected = float4( max( radiance, 0.0 ), mirror ? RT_REFLECT_MIRROR : min( hit.t, RT_REFLECT_FAR ) );
	}
	return reflected;
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
	const bool checkerboard = ( flags & 1u ) != 0;
	if ( !checkerboard ) {
		if ( all( id.xy < size ) ) {
			output[id.xy] = Reflect( id.xy, true );
		}
		return;
	}
	if ( id.y >= size.y ) {
		return;
	}
	const uint traced = ( phase + id.y ) & 1u;
	[loop]
	for ( uint k = 0; k < 2; k++ ) {
		const uint2 texel = uint2( id.x * 2 + ( traced ^ k ), id.y );
		if ( texel.x < size.x ) {
			output[texel] = Reflect( texel, k == 0 );
		}
	}
}
