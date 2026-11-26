// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	r_rtIndirect (R19): one bounce of light for the main view, a texel per stride pixels a side, the block's first
	pixel. A ray leaves the point the depth puts there by the cosine about the plane the depth holds; what it hits
	is lit by one of the lights that reach it, with a ray toward the light's disk, and by what it emits
	(include/rtLighting.hlsli). Written unexposed as include/rtBounce.hlsli names it, the frame's one ray:

	  amount  RGBA16F  a the luminance that came, rgb that times the ray's direction
	  chroma  RG16F    Co and Cg of it

	Three pairs of numbers per texel and frame (include/rtNoise.hlsli, at three places of the tile, each with its
	own step): the ray's direction, the light, the point of its disk. MULTISAMPLED reads sample 0.
*/

#include "../include/gbuffer.hlsli"
#include "../include/rtLighting.hlsli"
#include "../include/rtNoise.hlsli"
#include "../include/rtView.hlsli"
#include "../include/rtBounce.hlsli"

cbuffer IndirectParms : register( b0 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 origin;				// xyz the eye
	float4 viewport;			// x, y, width, height in the scene
	float4 limits;				// x the longest ray, y what a hit emits times this, z the most a ray brings (0: any)
	uint tlasSrv;
	uint recordsSrv;
	uint instancesSrv;
	uint verticesSrv;
	uint indexesSrv;
	uint skinnedSrv;
	uint lightsSrv;
	uint numLights;
	uint depthSrv;
	uint albedoSrv;				// GB3
	uint noiseSrv;
	uint noiseIndex;
	uint amountUav;
	uint chromaUav;
	uint linearSampler;
	uint clampSampler;
	int2 first;					// the first pixel in the scene
	uint2 size;					// texels
	uint stride;				// pixels a side per texel
	uint unused0;
	uint unused1;
	uint unused2;
};

// The cone of a ray that stands for a part of the half sphere: the hit's textures are read that coarse.
static const float BOUNCE_SPREAD = 0.05;

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	RWTexture2D<float4> amount = ResourceDescriptorHeap[amountUav];
	RWTexture2D<float2> chroma = ResourceDescriptorHeap[chromaUav];
	RT_SCENE_TEXTURE( float4 ) albedoMap = ResourceDescriptorHeap[albedoSrv];
	const int2 pixel = first + int2( id.xy ) * int( stride );
	const float drawn = RT_SCENE_LOAD( albedoMap, pixel ).a;

	float3 radiance = float3( 0.0, 0.0, 0.0 );
	float3 direction = float3( 0.0, 0.0, 1.0 );
	if ( GBufferDrawn( drawn ) ) {
		RtView view;
		view.clipToWorld = clipToWorld;
		view.eye = origin.xyz;
		view.viewport = viewport;
		view.depthSrv = depthSrv;
		const float depth = RtDepthAt( view, pixel );
		float3 position = RtPositionAt( view, pixel, depth );
		const float3 plane = RtDepthNormal( view, pixel, depth, position );
		// the weapon's depth hack: a quarter of the clip z is four times the distance, along the same line
		if ( GBufferWeapon( drawn ) ) {
			position = origin.xyz + ( position - origin.xyz ) * 0.25;
		}
		const float eyeDistance = length( position - origin.xyz );

		// the disk lifted onto the half sphere: as many rays per solid angle as its cosine
		const float2 disk = RtDisk( RtNoise( noiseSrv, id.xy + uint2( 11, 29 ), noiseIndex ) );
		const float3 other = ( abs( plane.z ) < 0.9 ) ? float3( 0.0, 0.0, 1.0 ) : float3( 1.0, 0.0, 0.0 );
		const float3 right = normalize( cross( other, plane ) );
		const float3 up = cross( plane, right );
		direction = normalize( right * disk.x + up * disk.y + plane * sqrt( max( 1.0 - dot( disk, disk ), 0.0 ) ) );

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
		lighting.maxLights = numLights;
		lighting.shadows = true;
		lighting.linearSampler = linearSampler;
		lighting.clampSampler = clampSampler;

		// off the surface by more than the depth's error there
		RayDesc ray;
		ray.Origin = position + plane * ( 0.02 + 0.001 * eyeDistance );
		ray.Direction = direction;
		ray.TMin = 0.0;
		ray.TMax = limits.x;
		const RtHit hit = RtTrace( heap, ray, RT_MASK_VISIBLE, BOUNCE_SPREAD, linearSampler );
		if ( hit.hit ) {
			const float pick = RtNoise( noiseSrv, id.xy + uint2( 37, 5 ), noiseIndex, 1 ).x;
			const float2 spot = RtDisk( RtNoise( noiseSrv, id.xy + uint2( 53, 47 ), noiseIndex, 2 ) );
			radiance = RtShadeSampled( heap, lighting, hit, ray, 0.0, BOUNCE_SPREAD, pick, spot, limits.y, false );
		}
	}

	float3 coded = RtBounceEncode( max( radiance, 0.0 ) );
	const float most = ( limits.z > 0.0 ) ? min( limits.z, RT_BOUNCE_MOST ) : RT_BOUNCE_MOST;
	if ( coded.x > most ) {
		coded *= most / coded.x;
	}
	amount[id.xy] = float4( direction * coded.x, coded.x );
	chroma[id.xy] = coded.yz;
}
