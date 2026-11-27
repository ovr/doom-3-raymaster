// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The guides of the indirect light's grid (include/rtGuide.hlsli), from the main view's depth: a texel per stride
	pixels a side, the first pixel of its block, the slope counted in texels. The normal is the plane's the depth
	holds (include/rtView.hlsli), not the G-buffer's bumped one: the bounce does not follow the bumps. MULTISAMPLED
	reads sample 0.
*/

#include "../include/rtGuide.hlsli"
#include "../include/rtView.hlsli"

cbuffer GuidesParms : register( b0 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 viewport;			// x, y, width, height in the scene: what the clip space maps to
	float4 origin;				// xyz the eye
	int2 first;					// the first pixel in the scene
	uint2 size;					// texels
	uint depthSrv;
	uint albedoSrv;				// GB3
	uint guidesUav;
	uint slopeUav;
	uint stride;				// pixels a side per texel
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

float4 ClipAt( SCENE_TEXTURE( float ) depthMap, int2 id ) {
	const int2 pixel = first + id * int( stride );
	const float2 ndc = ( float2( pixel ) + 0.5 - viewport.xy ) / viewport.zw * 2.0 - 1.0;
	return float4( ndc, SCENE_LOAD( depthMap, pixel ) * 2.0 - 1.0, 1.0 );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	SCENE_TEXTURE( float ) depthMap = ResourceDescriptorHeap[depthSrv];
	SCENE_TEXTURE( float4 ) albedoMap = ResourceDescriptorHeap[albedoSrv];
	RWTexture2D<uint> guides = ResourceDescriptorHeap[guidesUav];
	RWTexture2D<float2> slopes = ResourceDescriptorHeap[slopeUav];

	const int2 id = int2( dispatch.xy );
	const int2 pixel = first + id * int( stride );
	if ( !GBufferDrawn( SCENE_LOAD( albedoMap, pixel ).a ) ) {
		guides[dispatch.xy] = 0u;
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

	RtView view;
	view.clipToWorld = clipToWorld;
	view.eye = origin.xyz;
	view.viewport = viewport;
	view.depthSrv = depthSrv;
	guides[dispatch.xy] = RtPackGuide( EncodeOctahedral( RtDepthNormal( view, pixel, RtDepthAt( view, pixel ), position ) ), 1.0 / inverseW );
	slopes[dispatch.xy] = slope;
}
