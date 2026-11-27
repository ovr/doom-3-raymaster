// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The indirect light (R19) into the HDR scene's viewport: every pixel takes, of the four texels of the coarse grid
	around it, those of its surface (the plane of the texel's depth through the pixel's, the facing), each by the
	pixel's own bumped normal (include/rtBounce.hlsli), times its albedo, the scale and preExposure. The blend adds
	it, or puts it in the colour's place as a white surface takes it (r_rtDebug 7); the write mask keeps the alpha.
	Where none of the four is of the pixel's surface (something thinner than the grid) the twelve around them are
	asked, and where none of those is either, the four stand in whatever their surface. MULTISAMPLED reads sample 0.
*/

#include "../include/srgb.hlsli"
#include "../include/rtGuide.hlsli"
#include "../include/rtView.hlsli"
#include "../include/rtBounce.hlsli"

cbuffer UpsampleParms : register( b1 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 origin;				// xyz the eye
	float4 viewport;			// x, y, width, height in the scene
	int2 first;					// the grid's first pixel in the scene
	uint2 size;					// texels
	uint amountSrv;
	uint auxSrv;
	uint guidesSrv;
	uint slopeSrv;
	uint depthSrv;
	uint normalSrv;				// GB0
	uint albedoSrv;				// GB3
	uint stride;				// pixels a side per texel
	float scale;				// r_rtIndirectScale times preExposure
	uint flags;					// bit 0: without the albedo
	uint unused0;
	uint unused1;
};

// What the pixel takes of one texel of the grid.
struct Tap {
	float3	light;		// as the pixel's normal takes it
	float	same;		// 0..1: the texel lies in the pixel's plane and faces as it does
	bool	holds;
};

Tap TapOf( int2 q, float2 at, float clipW, float3 plane, float3 normal ) {
	Texture2D<float4> amountMap = ResourceDescriptorHeap[amountSrv];
	Texture2D<float4> auxMap = ResourceDescriptorHeap[auxSrv];
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	Tap tap;
	tap.light = float3( 0.0, 0.0, 0.0 );
	tap.same = 0.0;
	tap.holds = false;
	if ( any( q < 0 ) || any( q >= int2( size ) ) ) {
		return tap;
	}
	const float4 aux = auxMap.Load( int3( q, 0 ) );
	const RtGuide guide = RtUnpackGuide( guides.Load( int3( q, 0 ) ) );
	if ( !guide.drawn || aux.a < 0.5 ) {
		return tap;
	}
	const float4 amount = amountMap.Load( int3( q, 0 ) );
	const float facing = saturate( dot( plane, guide.normal ) );
	const float facing2 = facing * facing;
	const float facing4 = facing2 * facing2;
	tap.light = max( RtBounceDecode( amount.a, aux.rg ), 0.0 ) * RtBounceShare( normal, plane, amount );
	tap.same = RtPlaneWeight( guide.w, slopes.Load( int3( q, 0 ) ), at - float2( q ), clipW ) * facing4 * facing4;
	tap.holds = true;
	return tap;
}

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	RT_SCENE_TEXTURE( float4 ) albedoMap = ResourceDescriptorHeap[albedoSrv];
	const int2 pixel = int2( position.xy );
	const float4 albedo = RT_SCENE_LOAD( albedoMap, pixel );
	if ( !GBufferDrawn( albedo.a ) ) {
		return float4( 0.0, 0.0, 0.0, 0.0 );
	}
	RT_SCENE_TEXTURE( float2 ) normalMap = ResourceDescriptorHeap[normalSrv];

	RtView view;
	view.clipToWorld = clipToWorld;
	view.eye = origin.xyz;
	view.viewport = viewport;
	view.depthSrv = depthSrv;
	const float depth = RtDepthAt( view, pixel );
	const float2 ndc = ( float2( pixel ) + 0.5 - viewport.xy ) / viewport.zw * 2.0 - 1.0;
	const float clipW = 1.0 / dot( clipToWorld[3], float4( ndc, depth * 2.0 - 1.0, 1.0 ) );
	const float3 plane = RtDepthNormal( view, pixel, depth, RtPositionAt( view, pixel, depth ) );
	const float3 normal = DecodeOctahedral( RT_SCENE_LOAD( normalMap, pixel ) );

	// a texel is its block's first pixel
	const float2 at = float2( pixel - first ) / float( stride );
	const float2 base = floor( at );
	const float2 part = at - base;
	float3 sum = float3( 0.0, 0.0, 0.0 );
	float weight = 0.0;
	float3 near = float3( 0.0, 0.0, 0.0 );
	float nearWeight = 0.0;
	[unroll]
	for ( uint i = 0; i < 4; i++ ) {
		const int2 offset = int2( i & 1u, i >> 1 );
		const float w = ( ( offset.x != 0 ) ? part.x : 1.0 - part.x ) * ( ( offset.y != 0 ) ? part.y : 1.0 - part.y );
		if ( w > 0.0 ) {
			const Tap tap = TapOf( int2( base ) + offset, at, clipW, plane, normal );
			if ( tap.holds ) {
				sum += tap.light * ( w * tap.same );
				weight += w * tap.same;
				near += tap.light * w;
				nearWeight += w;
			}
		}
	}
	// something thinner than the grid: its own texels among the twelve around the four, the nearer the more
	if ( weight <= 1e-4 ) {
		sum = float3( 0.0, 0.0, 0.0 );
		weight = 0.0;
		for ( int v = -1; v <= 2; v++ ) {
			for ( int u = -1; u <= 2; u++ ) {
				if ( ( u == 0 || u == 1 ) && ( v == 0 || v == 1 ) ) {
					continue;
				}
				const int2 q = int2( base ) + int2( u, v );
				const Tap tap = TapOf( q, at, clipW, plane, normal );
				if ( tap.holds ) {
					const float2 off = at - float2( q );
					const float w = tap.same / ( 1.0 + dot( off, off ) );
					sum += tap.light * w;
					weight += w;
				}
			}
		}
	}
	float3 light = float3( 0.0, 0.0, 0.0 );
	if ( weight > 1e-4 ) {
		light = sum / weight;
	} else if ( nearWeight > 0.0 ) {
		light = near / nearWeight;
	}
	const float3 c = light * ( ( ( flags & 1u ) != 0 ) ? float3( 1.0, 1.0, 1.0 ) : albedo.rgb ) * scale;
	return float4( min( max( c, 0.0 ), HALF_MAX ), 0.0 );
}
