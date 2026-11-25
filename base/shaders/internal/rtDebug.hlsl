// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	r_rtDebug (RayTracingSettings.h): a ray per pixel of the main view from the eye through the pixel's centre,
	from the near plane on, against the GPU scene (include/raytracing.hlsli), drawn over the output after the
	resolve. Rows count up from the bottom as GL's, so the pixel's clip position is its place in the viewport.
	PRIMARY compares the hit with the scene's depth (sample 0 under MSAA) at the same pixel.
*/

#include "../include/raytracing.hlsli"

cbuffer DebugParms : register( b1 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 origin;				// xyz the eye, w the radians per pixel
	float4 viewport;			// x, y, width, height in the output
	uint tlasSrv;
	uint recordsSrv;
	uint instancesSrv;
	uint verticesSrv;
	uint indexesSrv;
	uint skinnedSrv;
	uint depthSrv;
	uint mode;					// rtDebug_t
	uint linearSampler;
	uint3 unused;
};

static const float RAY_LENGTH = 131072.0;

float3 ClipToWorld( float4 clip ) {
	const float4 p = float4( dot( clipToWorld[0], clip ), dot( clipToWorld[1], clip ), dot( clipToWorld[2], clip ), dot( clipToWorld[3], clip ) );
	return p.xyz / p.w;
}

#if MULTISAMPLED
float SceneDepth( int2 p ) {
	Texture2DMS<float> depth = ResourceDescriptorHeap[depthSrv];
	return depth.Load( p, 0 );
}
#else
float SceneDepth( int2 p ) {
	Texture2D<float> depth = ResourceDescriptorHeap[depthSrv];
	return depth.Load( int3( p, 0 ) );
}
#endif

// A colour per instance that does not depend on the frame.
float3 InstanceColor( uint id ) {
	uint h = id * 0x9E3779B1u;
	h ^= h >> 15;
	h *= 0x85EBCA77u;
	h ^= h >> 13;
	return 0.2 + 0.8 * float3( h & 0xFF, ( h >> 8 ) & 0xFF, ( h >> 16 ) & 0xFF ) / 255.0;
}

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	const float2 ndc = ( position.xy - viewport.xy ) / viewport.zw * 2.0 - 1.0;
	const float3 nearPoint = ClipToWorld( float4( ndc, -1.0, 1.0 ) );
	RayDesc ray;
	ray.Origin = origin.xyz;
	ray.Direction = normalize( nearPoint - origin.xyz );
	ray.TMin = length( nearPoint - origin.xyz );
	ray.TMax = RAY_LENGTH;

	RtHeap heap;
	heap.tlas = tlasSrv;
	heap.records = recordsSrv;
	heap.instances = instancesSrv;
	heap.vertices = verticesSrv;
	heap.indexes = indexesSrv;
	heap.skinned = skinnedSrv;
	const RtHit hit = RtTrace( heap, ray, RT_MASK_VISIBLE, origin.w, linearSampler );

	float3 c = float3( 0.0, 0.0, 0.0 );
	if ( mode == 2 ) {
		const float depth = SceneDepth( int2( position.xy ) );
		const bool drawn = depth < 1.0;
		const float drawnT = drawn ? length( ClipToWorld( float4( ndc, depth * 2.0 - 1.0, 1.0 ) ) - origin.xyz ) : RAY_LENGTH;
		// the depth's precision and the last bits of two transforms of the same vertices
		const float tolerance = 0.25 + 0.002 * drawnT;
		if ( drawn && hit.t > drawnT + tolerance ) {
			c = float3( 1.0, 0.0, 0.0 );
		} else if ( hit.hit && hit.t < drawnT - tolerance ) {
			c = float3( 0.0, 0.3, 1.0 );
		} else if ( hit.hit ) {
			const RtGeometry g = RtLoadGeometry( heap, hit.record );
			const RtSurface s = RtSurfaceOf( heap, g, hit.instanceIndex, hit.primitive, hit.barycentrics, hit.objectToWorld, hit.worldToObject );
			const float facing = abs( dot( s.faceNormal, ray.Direction ) );
			const bool backFace = !hit.frontFace && ( g.flags & RT_GEO_TWO_SIDED ) == 0;
			c = ( backFace ? float3( 1.0, 0.85, 0.0 ) : float3( 0.8, 0.8, 0.8 ) ) * ( 0.3 + 0.7 * facing );
		}
	} else if ( hit.hit ) {
		const RtGeometry g = RtLoadGeometry( heap, hit.record );
		const RtSurface s = RtSurfaceOf( heap, g, hit.instanceIndex, hit.primitive, hit.barycentrics, hit.objectToWorld, hit.worldToObject );
		if ( mode == 1 ) {
			c = InstanceColor( hit.instanceId ) * ( 0.3 + 0.7 * abs( dot( s.faceNormal, ray.Direction ) ) );
		} else if ( mode == 3 ) {
			c = 0.5 + 0.5 * s.normal;
		} else if ( mode == 4 ) {
			const float footprint = RtFootprint( hit.t, origin.w, ray.Direction, s.faceNormal );
			c = ( ( g.flags & RT_GEO_NO_SHADING ) != 0 ) ? float3( 0.25, 0.25, 0.25 )
				: saturate( RtDiffuse( g, s, footprint, linearSampler ) );
		}
	}
	return float4( c, 1.0 );
}
