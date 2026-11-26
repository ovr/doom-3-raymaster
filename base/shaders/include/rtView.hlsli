// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The main view's depth as a ray traced pass reads it: the point a pixel shows and the plane it lies in. The
	scene's rows count up from the bottom as GL's. MULTISAMPLED reads sample 0.
*/

#if MULTISAMPLED
#define RT_SCENE_TEXTURE( type ) Texture2DMS<type>
#define RT_SCENE_LOAD( texture, p ) texture.Load( p, 0 )
#else
#define RT_SCENE_TEXTURE( type ) Texture2D<type>
#define RT_SCENE_LOAD( texture, p ) texture.Load( int3( p, 0 ) )
#endif

struct RtView {
	float4	clipToWorld[4];	// rows: GL's clip space to world
	float3	eye;
	float4	viewport;		// x, y, width, height in the scene
	uint	depthSrv;
};

float RtDepthAt( RtView view, int2 pixel ) {
	RT_SCENE_TEXTURE( float ) depth = ResourceDescriptorHeap[view.depthSrv];
	return RT_SCENE_LOAD( depth, pixel );
}

float3 RtPositionAt( RtView view, int2 pixel, float depth ) {
	const float2 ndc = ( float2( pixel ) + 0.5 - view.viewport.xy ) / view.viewport.zw * 2.0 - 1.0;
	const float4 clip = float4( ndc, depth * 2.0 - 1.0, 1.0 );
	const float4 p = float4( dot( view.clipToWorld[0], clip ), dot( view.clipToWorld[1], clip ), dot( view.clipToWorld[2], clip ),
		dot( view.clipToWorld[3], clip ) );
	return p.xyz / p.w;
}

// The plane the depth holds at pixel, toward the eye: on each axis the neighbour nearer in depth, as across an edge
// the other side is farther.
float3 RtDepthNormal( RtView view, int2 pixel, float depth, float3 position ) {
	const int2 first = int2( view.viewport.xy );
	const int2 last = first + int2( view.viewport.zw ) - 1;
	float3 axes[2];
	[unroll]
	for ( uint a = 0; a < 2; a++ ) {
		const int2 step = ( a == 0 ) ? int2( 1, 0 ) : int2( 0, 1 );
		const int2 plus = min( pixel + step, last );
		const int2 minus = max( pixel - step, first );
		const float plusDepth = RtDepthAt( view, plus );
		const float minusDepth = RtDepthAt( view, minus );
		const bool hasPlus = any( plus != pixel );
		const bool takePlus = hasPlus && ( all( minus == pixel ) || abs( plusDepth - depth ) <= abs( minusDepth - depth ) );
		axes[a] = takePlus ? RtPositionAt( view, plus, plusDepth ) - position : position - RtPositionAt( view, minus, minusDepth );
	}
	const float3 n = cross( axes[0], axes[1] );
	const float size = length( n );
	if ( size <= 0.0 ) {
		return normalize( view.eye - position );
	}
	return ( dot( n, view.eye - position ) < 0.0 ) ? -n / size : n / size;
}
