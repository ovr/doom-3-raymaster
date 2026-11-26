// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The indirect light over the frames (R19, R18): the frame's ray (internal/rtIndirect.hlsl) joins the means its
	texel had where it was the frame before, found by the G-buffer's motion and taken only from the same surface
	(include/rtDenoise.hlsli; the guides hold the depth's plane). include/rtBounce.hlsli names what is kept.

	  amount, chroma  in   the frame's ray
	  amount, aux     out  the means, the luminance squared and the frames in them (at most maxHistory): the history
	  output          out  r, g the mean Co and Cg, b the variance of the mean luminance, a the frames: with the
	                       amount, what the spatial filter works on

	Unlike a shadow's edge, this light has none to keep: nothing is cut, and a change of the lights shows as fast
	as the history is long. Fewer than 4 frames tell no variance: the neighbours of this frame do then. A hole the
	guides bridged keeps its history, and so does every texel of a frame without a ray (flags bit 2): what the
	history has stands, uncounted, and a texel without one holds nothing.
*/

#include "../include/rtDenoise.hlsli"
#include "../include/rtBounce.hlsli"

cbuffer IndirectTemporalParms : register( b0 ) {
	uint2 size;				// texels
	uint amountSrv;
	uint chromaSrv;
	uint guidesSrv;
	uint reprojectSrv;
	uint slopeSrv;
	uint prevGuidesSrv;
	uint prevAmountSrv;
	uint prevAuxSrv;
	uint amountUav;
	uint auxUav;
	uint outputUav;
	uint flags;				// bit 0: the three of the frame before are there; 2: no ray this frame
	float maxHistory;
	float leastStep;		// RtAverage's: the last bit of the history's format over its value, or 0
};

static const float YOUNG = 4.0;		// frames a history needs to tell its variance

struct Held {
	float4	amount;
	float2	chroma;
	float	noise;		// the variance of one frame's luminance
	float	frames;		// 0: the texel has no history
};

// The four texels around where the texel's centre was the frame before, each only of its surface, as differences
// to the first one: equal ones give their value to the bit. Their variances are mixed, not their second moments.
Held ReadHistory( int2 texel, RtGuide guide, float2 slope ) {
	Texture2D<float4> reproject = ResourceDescriptorHeap[reprojectSrv];
	Texture2D<uint> prevGuides = ResourceDescriptorHeap[prevGuidesSrv];
	Texture2D<float4> prevAmount = ResourceDescriptorHeap[prevAmountSrv];
	Texture2D<float4> prevAux = ResourceDescriptorHeap[prevAuxSrv];
	Held held;
	held.amount = float4( 0.0, 0.0, 0.0, 0.0 );
	held.chroma = float2( 0.0, 0.0 );
	held.noise = 0.0;
	held.frames = 0.0;
	const float3 moved = reproject.Load( int3( texel, 0 ) ).xyz;
	const float2 at = float2( texel ) - moved.xy;
	const float2 base = floor( at );
	const float2 part = at - base;
	const float expected = guide.w * moved.z;
	const float tolerance = 0.02 + abs( slope.x ) + abs( slope.y );
	float4 firstAmount = float4( 0.0, 0.0, 0.0, 0.0 );
	float4 first = float4( 0.0, 0.0, 0.0, 0.0 );	// chroma, noise, frames
	float4 sumAmount = float4( 0.0, 0.0, 0.0, 0.0 );
	float4 sum = float4( 0.0, 0.0, 0.0, 0.0 );
	float weight = 0.0;
	[unroll]
	for ( uint tap = 0; tap < 4; tap++ ) {
		const int2 offset = int2( tap & 1u, tap >> 1 );
		const float w = ( ( offset.x != 0 ) ? part.x : 1.0 - part.x ) * ( ( offset.y != 0 ) ? part.y : 1.0 - part.y );
		const int2 q = int2( base ) + offset;
		if ( w > 0.0 && all( q >= 0 ) && all( q < int2( size ) ) ) {
			const float4 aux = prevAux.Load( int3( q, 0 ) );
			if ( aux.a >= 1.0 && RtSameSurface( guide, expected, tolerance, RtUnpackGuide( prevGuides.Load( int3( q, 0 ) ) ) ) ) {
				const float4 kept = prevAmount.Load( int3( q, 0 ) );
				const float4 rest = float4( aux.rg, aux.b - kept.a * kept.a, aux.a );
				if ( weight == 0.0 ) {
					firstAmount = kept;
					first = rest;
				}
				sumAmount += w * ( kept - firstAmount );
				sum += w * ( rest - first );
				weight += w;
			}
		}
	}
	if ( weight > 0.0 ) {
		const float4 rest = first + sum / weight;
		held.amount = firstAmount + sumAmount / weight;
		held.chroma = rest.rg;
		held.noise = max( rest.b, 0.0 );
		held.frames = rest.a;
	}
	return held;
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	RWTexture2D<float4> amountOut = ResourceDescriptorHeap[amountUav];
	RWTexture2D<float4> auxOut = ResourceDescriptorHeap[auxUav];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];

	const int2 texel = int2( dispatch.xy );
	const uint packed = guides.Load( int3( texel, 0 ) );
	const bool past = ( flags & 1u ) != 0;
	const bool hold = ( flags & 4u ) != 0;
	if ( ( packed & RT_GUIDE_DRAWN ) == 0 ) {
		amountOut[dispatch.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		auxOut[dispatch.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		output[dispatch.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	const RtGuide guide = RtUnpackGuide( packed );
	const float2 slope = slopes.Load( int3( texel, 0 ) );

	Held held;
	held.amount = float4( 0.0, 0.0, 0.0, 0.0 );
	held.chroma = float2( 0.0, 0.0 );
	held.noise = 0.0;
	held.frames = 0.0;
	bool keep = hold;
	if ( past && guide.bridged ) {
		Texture2D<float4> prevAmount = ResourceDescriptorHeap[prevAmountSrv];
		Texture2D<float4> prevAux = ResourceDescriptorHeap[prevAuxSrv];
		const float4 aux = prevAux.Load( int3( texel, 0 ) );
		if ( aux.a >= 1.0 ) {
			held.amount = prevAmount.Load( int3( texel, 0 ) );
			held.chroma = aux.rg;
			held.noise = max( aux.b - held.amount.a * held.amount.a, 0.0 );
			held.frames = aux.a;
		}
		keep = true;
	} else if ( past ) {
		held = ReadHistory( texel, guide, slope );
	}
	// nothing was traced here and nothing is kept: the next frame starts it anew
	if ( keep && held.frames < 1.0 ) {
		amountOut[dispatch.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		auxOut[dispatch.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		output[dispatch.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}

	float4 amount = held.amount;
	float2 chroma = held.chroma;
	float second = held.amount.a * held.amount.a + held.noise;
	const float count = keep ? held.frames : min( held.frames + 1.0, maxHistory );
	float around = 0.0;		// the variance the neighbours of this frame show
	if ( !keep ) {
		Texture2D<float4> amountMap = ResourceDescriptorHeap[amountSrv];
		Texture2D<float2> chromaMap = ResourceDescriptorHeap[chromaSrv];
		const float4 now = amountMap.Load( int3( texel, 0 ) );
		const float blend = 1.0 / count;
		amount.rgb = RtAverage( amount.rgb, now.rgb, blend, leastStep );
		amount.a = RtAverage( amount.a, now.a, blend, leastStep );
		chroma = RtAverage( float3( chroma, 0.0 ), float3( chromaMap.Load( int3( texel, 0 ) ), 0.0 ), blend, leastStep ).xy;
		second = RtAverage( second, now.a * now.a, blend, leastStep );
		if ( count < YOUNG ) {
			float sum = 0.0;
			float squares = 0.0;
			float taken = 0.0;
			for ( int y = -2; y <= 2; y++ ) {
				for ( int x = -2; x <= 2; x++ ) {
					const int2 q = texel + int2( x, y );
					if ( all( q >= 0 ) && all( q < int2( size ) ) ) {
						const RtGuide other = RtUnpackGuide( guides.Load( int3( q, 0 ) ) );
						if ( other.drawn && RtPlaneWeight( guide.w, slope, float2( x, y ), other.w ) > 0.5 && dot( guide.normal, other.normal ) > 0.6 ) {
							const float l = amountMap.Load( int3( q, 0 ) ).a;
							sum += l;
							squares += l * l;
							taken += 1.0;
						}
					}
				}
			}
			around = max( squares / taken - ( sum / taken ) * ( sum / taken ), 0.0 );
		}
	}
	// what the next frame reads; the reference keeps its means in 32 bits
	if ( leastStep > 0.0 ) {
		amount = float4( RtHalf( amount.rgb ), RtHalf( amount.a ) );
		chroma = RtHalf( float3( chroma, 0.0 ) ).xy;
		second = RtHalf( second );
	}
	// half floats round the two moments apart by up to this much: no variance yet
	const float variance = max( max( second - amount.a * amount.a - 5e-4 * second, 0.0 ), around );
	amountOut[dispatch.xy] = amount;
	auxOut[dispatch.xy] = float4( chroma, second, count );
	// of the mean, not of one frame
	output[dispatch.xy] = float4( chroma, variance / max( count, 1.0 ), count );
}
