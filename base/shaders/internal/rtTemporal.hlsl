// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	A ray traced signal over the frames (R18): the frame's value joins the average its pixel had where it was the
	frame before, found by the G-buffer's motion and taken only from the same surface (include/rtDenoise.hlsli).

	  signal   in   rgb the frame's value in 0..1, a a measure free of noise of what it is a part of (for the
	                shadows: the unshadowed luminance), exposed; 0: a part of nothing
	  colour   out  rgb the average, a the frames in it (at most maxHistory)
	  aux      out  r the average of the luminance squared, g the measure, unexposed
	  output   out  rgb the average, a the variance of its luminance: what the spatial filter works on

	A pixel whose measure is 0 (no traced light reaches it: a bump that faces away, the floor beyond the light) holds
	no value: no neighbour and no later frame takes it, and its variance is -1. Where the measure left the range
	of the history's texels by a quarter (a light went on, off or dim) the history is of another signal and is
	dropped.

	What keeps an edge without noise sharp (a plain average, flags bit 1, does neither):
	  a side   of the four texels, one of 4 frames or more that is farther from the frame's value than its noise
	           explains, and much farther than the nearest, lies across an edge and is left out: a moving eye
	           mixes no edge. Their variances are mixed, not their second moments, so a gradient is no noise.
	  the cut  an average outside the range of the pixel's 3 x 3 or 5 x 5 neighbours of this frame, by more than
	           the noise of its frames and of that many neighbours explains, is of a shadow that moved on. It is
	           cut to the range and, by as much, becomes the frame's value with the neighbours' variance.

	What the cut cannot tell, a history inside the range that is still of another signal (a penumbra where an
	umbra was), the gradient does (flags bit 3, internal/rtGradient.hlsl): where the frame's values of the tiles
	around the pixel lie farther from their histories than the noise of both explains, by as much the history
	is shortened, down to none. That costs noise for some frames and never a wrong value.

	Fewer than 4 frames tell no variance: the neighbours do then. A hole the guides bridged keeps its history.
	So does every pixel of a frame that holds no trace (flags bit 2: its signal is all 1): what the history has
	of it stands, uncut and uncounted, and a pixel without one holds no value.
*/

#include "../include/rtDenoise.hlsli"

cbuffer TemporalParms : register( b0 ) {
	uint2 size;				// of the viewport
	uint signalSrv;
	uint guidesSrv;
	uint reprojectSrv;
	uint slopeSrv;
	uint prevGuidesSrv;
	uint prevColorSrv;
	uint prevAuxSrv;
	uint colorUav;
	uint auxUav;
	uint outputUav;
	// bit 0: the frame before's guides, colour and aux are there; 1: a plain average; 2: no trace; 3: the gradient is there
	uint flags;
	float maxHistory;
	float invPreExposure;
	float leastStep;		// RtAverage's: the last bit of the colour's format over its value, or 0
	uint gradientSrv;		// a texel per tile
};

static const float CUT_SPREAD = 2.0;		// deviations of n neighbours' mean the history may lie outside their range
static const float GRADIENT_SPREAD = 3.0;	// deviations of the tiles' noise their frame may lie from their histories
static const float GRADIENT_PIXELS = 8.0;	// pixels that must count around the pixel

// How much of the pixel's history is of another signal, 0..1: the frame's values against the histories' over
// the four tiles around it, each by how near its centre is, so that no tile's border shows.
float Changed( int2 pixel ) {
	Texture2D<float4> gradient = ResourceDescriptorHeap[gradientSrv];
	const int2 tiles = int2( ( size + RT_TILE - 1 ) / RT_TILE );
	const float2 at = ( float2( pixel ) + 0.5 ) / float( RT_TILE ) - 0.5;
	const float2 base = floor( at );
	const float2 part = at - base;
	float difference = 0.0;
	float counted = 0.0;
	float variance = 0.0;
	[unroll]
	for ( uint tap = 0; tap < 4; tap++ ) {
		const int2 offset = int2( tap & 1u, tap >> 1 );
		const int2 tile = int2( base ) + offset;
		const float w = ( ( offset.x != 0 ) ? part.x : 1.0 - part.x ) * ( ( offset.y != 0 ) ? part.y : 1.0 - part.y );
		if ( all( tile >= 0 ) && all( tile < tiles ) ) {
			const float3 held = gradient.Load( int3( tile, 0 ) ).rgb;
			difference += w * held.r;
			counted += w * held.g;
			variance += w * w * held.b;
		}
	}
	// a tile holds its sums over its pixels
	const float pixels = float( RT_TILE * RT_TILE );
	if ( counted * pixels < GRADIENT_PIXELS ) {
		return 0.0;
	}
	// of the mean of the pixels that count
	const float deviation = sqrt( variance / pixels ) / counted;
	return saturate( abs( difference ) / ( counted * max( GRADIENT_SPREAD * deviation, RT_EDGE_STEP ) ) - 1.0 );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	Texture2D<float4> signal = ResourceDescriptorHeap[signalSrv];
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	RWTexture2D<float4> color = ResourceDescriptorHeap[colorUav];
	RWTexture2D<float2> aux = ResourceDescriptorHeap[auxUav];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];

	const int2 pixel = int2( dispatch.xy );
	const float4 now = signal.Load( int3( pixel, 0 ) );
	const float3 value = saturate( now.rgb );
	const float luminance = dot( value, RT_LUMA );
	const float measure = now.a * invPreExposure;
	const uint packed = guides.Load( int3( pixel, 0 ) );
	const bool past = ( flags & 1u ) != 0 && ( packed & RT_GUIDE_DRAWN ) != 0;
	const bool plain = ( flags & 2u ) != 0;
	const bool hold = ( flags & 4u ) != 0;

	float3 mean = float3( 0.0, 0.0, 0.0 );
	float second = 0.0;
	float frames = 0.0;
	float measured = measure;
	bool keep = false;
	if ( past && ( packed & RT_GUIDE_BRIDGED ) != 0 ) {
		Texture2D<float4> prevColor = ResourceDescriptorHeap[prevColorSrv];
		Texture2D<float2> prevAux = ResourceDescriptorHeap[prevAuxSrv];
		const float4 held = prevColor.Load( int3( pixel, 0 ) );
		if ( held.a >= 1.0 ) {
			const float2 heldAux = prevAux.Load( int3( pixel, 0 ) );
			mean = held.rgb;
			frames = held.a;
			second = heldAux.x;
			measured = heldAux.y;
			keep = true;
		}
	}
	// Nothing drawn, or the measure is 0 (no traced light reaches the pixel): its value is a part of nothing. It
	// gets no history, and the negative variance tells the spatial filter to leave it out.
	if ( ( packed & RT_GUIDE_DRAWN ) == 0 || ( now.a <= 0.0 && !keep ) ) {
		color[dispatch.xy] = float4( value, 0.0 );
		aux[dispatch.xy] = float2( 0.0, 0.0 );
		output[dispatch.xy] = float4( value, -1.0 );
		return;
	}
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	const RtGuide guide = RtUnpackGuide( packed );
	const float2 slope = slopes.Load( int3( pixel, 0 ) );

	// The pixel's surface around it in this frame: the range its value lies in, of the 3 x 3 and of the 5 x 5
	// neighbours, and how far the luminance spreads (5 x 5).
	float3 low = value;
	float3 high = value;
	float3 wideLow = value;
	float3 wideHigh = value;
	float sum = 0.0;
	float squares = 0.0;
	float inner = 0.0;	// neighbours taken of the 3 x 3
	float taken = 0.0;	// and of the 5 x 5
	for ( int y = -2; y <= 2; y++ ) {
		for ( int x = -2; x <= 2; x++ ) {
			const int2 q = pixel + int2( x, y );
			if ( all( q >= 0 ) && all( q < int2( size ) ) ) {
				const RtGuide other = RtUnpackGuide( guides.Load( int3( q, 0 ) ) );
				if ( other.drawn && RtPlaneWeight( guide.w, slope, float2( x, y ), other.w ) > 0.5 && dot( guide.normal, other.normal ) > 0.6 ) {
					const float4 around = signal.Load( int3( q, 0 ) );
					if ( around.a > 0.0 ) {
						const float3 tap = saturate( around.rgb );
						const float l = dot( tap, RT_LUMA );
						if ( abs( x ) <= 1 && abs( y ) <= 1 ) {
							low = min( low, tap );
							high = max( high, tap );
							inner += 1.0;
						}
						wideLow = min( wideLow, tap );
						wideHigh = max( wideHigh, tap );
						sum += l;
						squares += l * l;
						taken += 1.0;
					}
				}
			}
		}
	}

	if ( past && !guide.bridged ) {
		Texture2D<uint> prevGuides = ResourceDescriptorHeap[prevGuidesSrv];
		Texture2D<float4> prevColor = ResourceDescriptorHeap[prevColorSrv];
		Texture2D<float2> prevAux = ResourceDescriptorHeap[prevAuxSrv];
		Texture2D<float4> reproject = ResourceDescriptorHeap[reprojectSrv];
		const RtHeld held = RtReadHistory( pixel, size, guide, slope, value, measure, !plain && !hold, reproject, prevGuides, prevColor, prevAux );
		if ( held.frames > 0.0 ) {
			mean = held.mean;
			second = held.second;
			frames = held.frames;
			// A history outside what the neighbours hold now is of a shadow that moved on. Its own noise explains
			// CUT_SPREAD deviations between its mean over its frames and the mean of n neighbours: the more
			// of them agree, the less it may differ. A history of a few frames knows no noise yet.
			const float mixed = dot( mean, RT_LUMA );
			const float known = RtKnownNoise( mixed, second );
			const bool whole = plain || hold || frames < RT_CUT_FRAMES;
			const float spread = whole ? 1.0 : CUT_SPREAD * sqrt( known * ( 1.0 / inner + 1.0 / frames ) );
			const float wideSpread = whole ? 1.0 : CUT_SPREAD * sqrt( known * ( 1.0 / taken + 1.0 / frames ) );
			const float3 inside = clamp( mean, max( low - spread, wideLow - wideSpread ), min( high + spread, wideHigh + wideSpread ) );
			const float3 cut = abs( mean - inside );
			const float again = saturate( RT_CUT_FRAMES * max( cut.r, max( cut.g, cut.b ) ) );
			if ( again > 0.0 ) {
				// As far as it is cut it is a new pixel: the frame's value, the variance its neighbours show
				// (none where they agree: a moved shadow without noise is not blurred), and frames enough
				// to go by that variance.
				const float around = max( squares / taken - ( sum / taken ) * ( sum / taken ), 0.0 );
				mean = lerp( inside, value, again );
				const float after = dot( mean, RT_LUMA );
				second = after * after + lerp( known, around, again );
				frames = max( frames * ( 1.0 - again ), RT_CUT_FRAMES - 1.0 );
			}
			// What is left of a history of another signal joins the frame as that many frames. After the cut, which
			// needs the history's length to tell what its noise explains.
			if ( ( flags & 8u ) != 0 ) {
				frames *= 1.0 - Changed( pixel );
			}
		}
	}

	// A frame without a trace adds nothing: the history stands. Without one the pixel holds no value, as one no
	// light reaches: the next frame starts it anew.
	if ( hold && !keep ) {
		if ( frames < 1.0 ) {
			color[dispatch.xy] = float4( value, 0.0 );
			aux[dispatch.xy] = float2( 0.0, 0.0 );
			output[dispatch.xy] = float4( value, -1.0 );
			return;
		}
		keep = true;
	}

	const float count = keep ? frames : min( frames + 1.0, maxHistory );
	if ( !keep ) {
		mean = RtAverage( mean, value, 1.0 / count, leastStep );
		second = RtAverage( second, luminance * luminance, 1.0 / count, leastStep );
	}
	// what the next frame reads; the reference keeps its average in 32 bits
	const float3 stored = RtHalf( mean );
	if ( leastStep > 0.0 ) {
		mean = stored;
	}
	second = RtHalf( second );
	const float average = dot( stored, RT_LUMA );
	// half floats round the two moments apart by up to this much: no variance yet
	float variance = max( second - average * average - 5e-4 * second, 0.0 );
	if ( count < 4.0 && taken > 0.5 ) {
		// too few frames to tell; the neighbours of this frame do (the pixel itself is one of them)
		variance = max( variance, squares / taken - ( sum / taken ) * ( sum / taken ) );
	}
	color[dispatch.xy] = float4( mean, count );
	aux[dispatch.xy] = float2( second, measured );
	// of the average, not of one frame
	output[dispatch.xy] = float4( stored, variance / max( count, 1.0 ) );
}
