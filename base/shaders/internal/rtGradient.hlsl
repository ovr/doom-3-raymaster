// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The temporal gradient of a ray traced signal (R18): per tile of RT_TILE x RT_TILE pixels, how far the frame's
	values lie from what the history holds of the same pixels, and the noise that explains a difference.
	internal/rtTemporal.hlsl shortens the histories where the two are farther apart than that: a soft shadow that
	moved on leaves its umbra, a history without noise, inside the new penumbra, whose neighbours hold every value,
	so nothing there is cut.

	  output  out  per tile, each over its 64 pixels: r the sum of the luminance's differences, frame - history,
	               g the pixels that count, b the sum of the variances of their differences

	A pixel counts with a history, read as rtTemporal reads it. One of RT_CUT_FRAMES frames or more tells its
	noise; a younger one takes the tile's spread of the frame's values for it, which is no less. An old one does
	not count beside a hard edge of the history, a texel 2 pixels away that holds another value than the noise of
	the two explains: there the frame differs because the edge moved a pixel, which the cut answers and keeps
	sharp. A group is a tile; its threads' votes are summed by one of them, in their order.
*/

#include "../include/rtDenoise.hlsli"

cbuffer GradientParms : register( b0 ) {
	uint2 size;				// of the viewport
	uint signalSrv;
	uint guidesSrv;
	uint reprojectSrv;
	uint slopeSrv;
	uint prevGuidesSrv;
	uint prevColorSrv;
	uint prevAuxSrv;
	uint outputUav;			// RGBA16F, a texel per tile
	float invPreExposure;
	uint unused;
};

static const float EDGE_SPREAD = 3.0;	// deviations of two histories' noise that an edge between them exceeds

struct Vote {
	float	difference;		// of the luminance, frame - history
	float	counts;			// 1 or 0
	float	noise;			// the variance of the difference that the history tells
	float	young;			// or how many times the tile's spread it is
	float	light;			// the frame's luminance
	float	reached;		// 1 where the frame holds a value
};

groupshared float4 votes[RT_TILE * RT_TILE];	// difference, counts, noise, young
groupshared float2 lights[RT_TILE * RT_TILE];	// light, reached

Vote VoteOf( int2 pixel ) {
	Vote vote;
	vote.difference = 0.0;
	vote.counts = 0.0;
	vote.noise = 0.0;
	vote.young = 0.0;
	vote.light = 0.0;
	vote.reached = 0.0;
	Texture2D<float4> signal = ResourceDescriptorHeap[signalSrv];
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	const uint packed = guides.Load( int3( pixel, 0 ) );
	const float4 now = signal.Load( int3( pixel, 0 ) );
	// nothing drawn, a hole that keeps its history as it is, or a part of nothing
	if ( ( packed & RT_GUIDE_DRAWN ) == 0 || ( packed & RT_GUIDE_BRIDGED ) != 0 || now.a <= 0.0 ) {
		return vote;
	}
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	Texture2D<float4> reproject = ResourceDescriptorHeap[reprojectSrv];
	Texture2D<uint> prevGuides = ResourceDescriptorHeap[prevGuidesSrv];
	Texture2D<float4> prevColor = ResourceDescriptorHeap[prevColorSrv];
	Texture2D<float2> prevAux = ResourceDescriptorHeap[prevAuxSrv];
	const float3 value = saturate( now.rgb );
	vote.light = dot( value, RT_LUMA );
	vote.reached = 1.0;
	const RtHeld held = RtReadHistory( pixel, size, RtUnpackGuide( packed ), slopes.Load( int3( pixel, 0 ) ), value, now.a * invPreExposure, true,
		reproject, prevGuides, prevColor, prevAux );
	if ( held.frames < 1.0 ) {
		return vote;
	}
	const float mixed = dot( held.mean, RT_LUMA );
	const float difference = vote.light - mixed;
	// the frame's sample is as noisy as the history's were, and the history's mean holds a part of that
	const float both = 1.0 + 1.0 / held.frames;
	if ( held.frames < RT_CUT_FRAMES ) {
		vote.difference = difference;
		vote.counts = 1.0;
		vote.young = both;
		return vote;
	}
	const float noise = RtKnownNoise( mixed, held.second );
	// only a pixel that differs asks; whether it counts hangs on the history alone, so the sum has no lean
	if ( abs( difference ) > RT_EDGE_STEP ) {
		const int2 was = int2( round( float2( pixel ) - reproject.Load( int3( pixel, 0 ) ).xy ) );
		for ( int y = -2; y <= 2; y += 2 ) {
			for ( int x = -2; x <= 2; x += 2 ) {
				const int2 q = was + int2( x, y );
				if ( ( x != 0 || y != 0 ) && all( q >= 0 ) && all( q < int2( size ) ) ) {
					const float4 other = prevColor.Load( int3( q, 0 ) );
					if ( other.a >= 1.0 ) {
						const float otherMean = dot( other.rgb, RT_LUMA );
						const float otherNoise = RtKnownNoise( otherMean, prevAux.Load( int3( q, 0 ) ).x );
						const float explained = EDGE_SPREAD * sqrt( noise / held.frames + otherNoise / other.a );
						if ( abs( otherMean - mixed ) > max( 2.0 * RT_EDGE_STEP, explained ) ) {
							return vote;
						}
					}
				}
			}
		}
	}
	vote.difference = difference;
	vote.counts = 1.0;
	vote.noise = noise * both;
	return vote;
}

[numthreads( RT_TILE, RT_TILE, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex ) {
	float4 mine = float4( 0.0, 0.0, 0.0, 0.0 );
	float2 light = float2( 0.0, 0.0 );
	if ( all( dispatch.xy < size ) ) {
		const Vote vote = VoteOf( int2( dispatch.xy ) );
		mine = float4( vote.difference, vote.counts, vote.noise, vote.young );
		light = float2( vote.light, vote.reached );
	}
	votes[index] = mine;
	lights[index] = light;
	GroupMemoryBarrierWithGroupSync();
	if ( index == 0 ) {
		float4 sum = float4( 0.0, 0.0, 0.0, 0.0 );
		float2 lit = float2( 0.0, 0.0 );
		float squares = 0.0;
		for ( uint i = 0; i < RT_TILE * RT_TILE; i++ ) {
			sum += votes[i];
			lit += lights[i];
			squares += lights[i].x * lights[i].x;
		}
		// how far the frame's values spread over the tile: their noise, and what the signal changes across it
		const float average = lit.x / max( lit.y, 1.0 );
		const float spread = max( squares / max( lit.y, 1.0 ) - average * average, 0.0 );
		RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
		output[group.xy] = float4( RtHalf( float3( sum.x, sum.y, sum.z + spread * sum.w ) / float( RT_TILE * RT_TILE ) ), 0.0 );
	}
}
