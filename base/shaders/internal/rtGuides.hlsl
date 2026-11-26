// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The guides of the main view's frame (include/rtDenoise.hlsli), from its depth and G-buffer, a texel per pixel of
	the viewport. Where the G-buffer holds nothing and the frame before held a surface, that surface's guide stays
	for this one frame, marked: a driver's hole does not end a history, and the sky ends it one frame late.
	MULTISAMPLED reads sample 0.
*/

#include "../include/rtDenoise.hlsli"

cbuffer GuidesParms : register( b0 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 prevClipW;			// the row of w of the frame before's world to clip
	float4 viewport;			// x, y, width, height in the scene: what the clip space maps to
	int2 first;					// the first pixel in the scene
	uint2 size;					// pixels
	uint depthSrv;
	uint normalSrv;				// GB0
	uint motionSrv;				// GB2
	uint albedoSrv;				// GB3
	uint prevGuidesSrv;
	uint guidesUav;
	uint reprojectUav;
	uint slopeUav;
	uint flags;					// bit 0: the frame before's guides are there
	uint unused0;
	uint unused1;
	uint unused2;
};

#if MULTISAMPLED
#define SCENE_TEXTURE( type ) Texture2DMS<type>
#define SCENE_LOAD( texture, p ) texture.Load( p, 0 )
#else
#define SCENE_TEXTURE( type ) Texture2D<type>
#define SCENE_LOAD( texture, p ) texture.Load( int3( p, 0 ) )
#endif

// A pixel of the viewport in clip space, w 1: clip to world of it is the point over its clip w.
float4 ClipAt( SCENE_TEXTURE( float ) depthMap, int2 id ) {
	const int2 pixel = first + id;
	const float2 ndc = ( float2( pixel ) + 0.5 - viewport.xy ) / viewport.zw * 2.0 - 1.0;
	return float4( ndc, SCENE_LOAD( depthMap, pixel ) * 2.0 - 1.0, 1.0 );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	SCENE_TEXTURE( float ) depthMap = ResourceDescriptorHeap[depthSrv];
	SCENE_TEXTURE( float2 ) normalMap = ResourceDescriptorHeap[normalSrv];
	SCENE_TEXTURE( float2 ) motionMap = ResourceDescriptorHeap[motionSrv];
	SCENE_TEXTURE( float4 ) albedoMap = ResourceDescriptorHeap[albedoSrv];
	RWTexture2D<uint> guides = ResourceDescriptorHeap[guidesUav];
	RWTexture2D<float4> reproject = ResourceDescriptorHeap[reprojectUav];
	RWTexture2D<float2> slopes = ResourceDescriptorHeap[slopeUav];

	const int2 id = int2( dispatch.xy );
	const int2 pixel = first + id;
	const float drawn = SCENE_LOAD( albedoMap, pixel ).a;
	if ( !GBufferDrawn( drawn ) ) {
		uint kept = 0;
		if ( ( flags & 1u ) != 0 ) {
			Texture2D<uint> prevGuides = ResourceDescriptorHeap[prevGuidesSrv];
			const uint before = prevGuides.Load( int3( id, 0 ) );
			kept = ( ( before & ( RT_GUIDE_DRAWN | RT_GUIDE_BRIDGED ) ) == RT_GUIDE_DRAWN ) ? ( before | RT_GUIDE_BRIDGED ) : 0u;
		}
		guides[dispatch.xy] = kept;
		reproject[dispatch.xy] = float4( 0.0, 0.0, 1.0, 0.0 );
		slopes[dispatch.xy] = float2( 0.0, 0.0 );
		return;
	}

	const float4 clip = ClipAt( depthMap, id );
	const float4 point4 = float4( dot( clipToWorld[0], clip ), dot( clipToWorld[1], clip ), dot( clipToWorld[2], clip ), dot( clipToWorld[3], clip ) );
	const float inverseW = point4.w;
	const float3 position = point4.xyz / inverseW;

	// per axis the neighbour nearer in depth: across an edge the other side is farther off the plane
	float2 slope = float2( 0.0, 0.0 );
	[unroll]
	for ( uint axis = 0; axis < 2; axis++ ) {
		const int2 step = ( axis == 0 ) ? int2( 1, 0 ) : int2( 0, 1 );
		const bool hasPlus = all( id + step < int2( size ) );
		const bool hasMinus = all( id - step >= 0 );
		const float toPlus = hasPlus ? dot( clipToWorld[3], ClipAt( depthMap, id + step ) ) - inverseW : 0.0;
		const float fromMinus = hasMinus ? inverseW - dot( clipToWorld[3], ClipAt( depthMap, id - step ) ) : 0.0;
		const float change = ( hasPlus && ( !hasMinus || abs( toPlus ) <= abs( fromMinus ) ) ) ? toPlus : fromMinus;
		const float part = clamp( change / inverseW, -0.1, 0.1 );
		if ( axis == 0 ) {
			slope.x = part;
		} else {
			slope.y = part;
		}
	}

	// The depth the point had if it stood still; what moves is within the test's tolerance. Under the weapon's
	// depth hack the depth is not the point's: it keeps its own.
	const float before = GBufferWeapon( drawn ) ? 1.0 : dot( prevClipW, float4( position, 1.0 ) ) * inverseW;
	guides[dispatch.xy] = RtPackGuide( SCENE_LOAD( normalMap, pixel ), 1.0 / inverseW );
	reproject[dispatch.xy] = float4( SCENE_LOAD( motionMap, pixel ), before, 0.0 );
	slopes[dispatch.xy] = slope;
}
