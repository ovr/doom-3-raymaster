// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	r_rtReflections (RayTracingSettings.h): a mirror ray per pixel of the main view whose G-buffer may reflect (F0
	above 0, roughness below the limit), from the point the depth puts there, about the G-buffer's normal, against the
	GPU scene; its hit lit as the lights light it (include/rtLighting.hlsli), times Schlick's Fresnel (capped by the
	smoothness, so rough surfaces do not turn to mirrors at grazing angles) and the fade toward the limit. Written
	unexposed, a texel per pixel of the viewport; rows count up from the bottom as GL's, as the scene's do.
	MULTISAMPLED reads sample 0.
*/

#include "../include/gbuffer.hlsli"
#include "../include/rtLighting.hlsli"

cbuffer ReflectParms : register( b0 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 origin;				// xyz the eye, w the radians per pixel
	float4 viewport;			// x, y, width, height in the scene
	float4 limits;				// x the longest ray, y the roughness that reflects nothing, z the fade below it
	uint tlasSrv;
	uint recordsSrv;
	uint instancesSrv;
	uint verticesSrv;
	uint indexesSrv;
	uint skinnedSrv;
	uint lightsSrv;
	uint numLights;
	uint depthSrv;
	uint normalSrv;
	uint materialSrv;
	uint outputUav;
	uint linearSampler;
	uint clampSampler;
	uint maxLights;
	uint shadows;
};

#if MULTISAMPLED
float LoadDepth( int2 p ) {
	Texture2DMS<float> depth = ResourceDescriptorHeap[depthSrv];
	return depth.Load( p, 0 );
}
float2 LoadNormal( int2 p ) {
	Texture2DMS<float2> normal = ResourceDescriptorHeap[normalSrv];
	return normal.Load( p, 0 );
}
float4 LoadMaterial( int2 p ) {
	Texture2DMS<float4> material = ResourceDescriptorHeap[materialSrv];
	return material.Load( p, 0 );
}
#else
float LoadDepth( int2 p ) {
	Texture2D<float> depth = ResourceDescriptorHeap[depthSrv];
	return depth.Load( int3( p, 0 ) );
}
float2 LoadNormal( int2 p ) {
	Texture2D<float2> normal = ResourceDescriptorHeap[normalSrv];
	return normal.Load( int3( p, 0 ) );
}
float4 LoadMaterial( int2 p ) {
	Texture2D<float4> material = ResourceDescriptorHeap[materialSrv];
	return material.Load( int3( p, 0 ) );
}
#endif

float3 PositionAt( int2 pixel, float depth ) {
	const float2 ndc = ( float2( pixel ) + 0.5 - viewport.xy ) / viewport.zw * 2.0 - 1.0;
	const float4 clip = float4( ndc, depth * 2.0 - 1.0, 1.0 );
	const float4 p = float4( dot( clipToWorld[0], clip ), dot( clipToWorld[1], clip ), dot( clipToWorld[2], clip ), dot( clipToWorld[3], clip ) );
	return p.xyz / p.w;
}

// The plane the depth holds at pixel, toward the eye: on each axis the neighbour nearer in depth, as across an edge
// the other side is farther.
float3 DepthNormal( int2 pixel, float depth, float3 position ) {
	const int2 first = int2( viewport.xy );
	const int2 last = first + int2( viewport.zw ) - 1;
	float3 axes[2];
	[unroll]
	for ( uint a = 0; a < 2; a++ ) {
		const int2 step = ( a == 0 ) ? int2( 1, 0 ) : int2( 0, 1 );
		const int2 plus = min( pixel + step, last );
		const int2 minus = max( pixel - step, first );
		const float plusDepth = LoadDepth( plus );
		const float minusDepth = LoadDepth( minus );
		const bool hasPlus = any( plus != pixel );
		const bool takePlus = hasPlus && ( all( minus == pixel ) || abs( plusDepth - depth ) <= abs( minusDepth - depth ) );
		axes[a] = takePlus ? PositionAt( plus, plusDepth ) - position : position - PositionAt( minus, minusDepth );
	}
	const float3 n = cross( axes[0], axes[1] );
	const float size = length( n );
	if ( size <= 0.0 ) {
		return normalize( origin.xyz - position );
	}
	return ( dot( n, origin.xyz - position ) < 0.0 ) ? -n / size : n / size;
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( id.x >= uint( viewport.z ) || id.y >= uint( viewport.w ) ) {
		return;
	}
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
	const int2 pixel = int2( viewport.xy ) + int2( id.xy );
	const float4 material = LoadMaterial( pixel );
	const float depth = LoadDepth( pixel );
	const float roughness = material.a;
	const float weight = saturate( ( limits.y - roughness ) / limits.z );

	float3 reflected = float3( 0.0, 0.0, 0.0 );
	if ( depth < 1.0 && weight > 0.0 && any( material.rgb > 0.0 ) ) {
		const float3 position = PositionAt( pixel, depth );
		const float3 normal = DecodeOctahedral( LoadNormal( pixel ) );
		const float3 plane = DepthNormal( pixel, depth, position );
		const float eyeDistance = length( position - origin.xyz );
		const float3 view = ( position - origin.xyz ) / max( eyeDistance, 1e-6 );
		// a bumped normal may turn the mirror ray into the surface: it stays above the plane the depth holds
		float3 direction = reflect( view, normal );
		const float above = dot( direction, plane );
		if ( above < 0.01 ) {
			direction = normalize( direction + plane * ( 0.01 - above ) );
		}
		const float3 f0 = material.rgb;
		const float3 fresnel = f0 + ( max( 1.0 - roughness, f0 ) - f0 ) * pow( 1.0 - saturate( dot( -view, normal ) ), 5.0 );

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
		const RtHit hit = RtTrace( heap, ray, RT_MASK_VISIBLE, origin.w, linearSampler );
		if ( hit.hit ) {
			reflected = RtShade( heap, lighting, hit, ray, eyeDistance, origin.w ) * fresnel * weight;
		}
	}
	output[id.xy] = float4( reflected, 1.0 );
}
