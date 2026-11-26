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

	Fewer than 4 frames tell no variance: the neighbours do then. A hole the guides bridged keeps its history.
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
	uint flags;				// bit 0: the frame before's guides, colour and aux are there; bit 1: a plain average
	float maxHistory;
	float invPreExposure;
	float leastStep;		// RtAverage's: the last bit of the colour's format over its value, or 0
};

static const float CUT_SPREAD = 2.0;	// deviations of n neighbours' mean the history may lie outside their range
static const float CUT_FRAMES = 4.0;	// frames a history needs to be cut, and to tell its variance
static const float EDGE_STEP = 1.0 / 16.0;	// the least step between the two sides of an edge
static const float EDGE_DEVIATIONS = 4.0;	// of a texel's noise its step must exceed

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
		const float3 moved = reproject.Load( int3( pixel, 0 ) ).xyz;
		// the scene counts its rows up, as the motion does: where the pixel's centre was, in texels
		const float2 at = float2( pixel ) - moved.xy;
		const float2 base = floor( at );
		const float2 part = at - base;
		const float expected = guide.w * moved.z;
		const float tolerance = 0.02 + abs( slope.x ) + abs( slope.y );
		float4 tapColor[4];
		float2 tapAux[4];
		float tapWeight[4];
		float tapFar[4];		// from the frame's value
		float nearest = 1.0;
		[unroll]
		for ( uint tap = 0; tap < 4; tap++ ) {
			const int2 offset = int2( tap & 1u, tap >> 1 );
			const float w = ( ( offset.x != 0 ) ? part.x : 1.0 - part.x ) * ( ( offset.y != 0 ) ? part.y : 1.0 - part.y );
			const int2 q = int2( base ) + offset;
			tapColor[tap] = float4( 0.0, 0.0, 0.0, 0.0 );
			tapAux[tap] = float2( 0.0, 0.0 );
			tapWeight[tap] = 0.0;
			tapFar[tap] = 0.0;
			if ( w > 0.0 && all( q >= 0 ) && all( q < int2( size ) ) ) {
				const RtGuide other = RtUnpackGuide( prevGuides.Load( int3( q, 0 ) ) );
				const float4 held = prevColor.Load( int3( q, 0 ) );
				// a texel without a frame held nothing: no light reached it
				if ( held.a >= 1.0 && RtSameSurface( guide, expected, tolerance, other ) ) {
					const float3 apart = abs( held.rgb - value );
					tapColor[tap] = held;
					tapAux[tap] = prevAux.Load( int3( q, 0 ) );
					tapWeight[tap] = w;
					tapFar[tap] = max( apart.r, max( apart.g, apart.b ) );
					nearest = min( nearest, tapFar[tap] );
				}
			}
		}
		float weight = 0.0;
		float least = 3.4e38;	// of the taps' measures
		float most = 0.0;
		// The taps as differences to the first one: equal ones give their value to the bit. Of the luminance it is
		// the variance that is mixed: the second moment of two texels that differ would count their difference as noise.
		float4 first = float4( 0.0, 0.0, 0.0, 0.0 );
		float firstNoise = 0.0;
		float noise = 0.0;
		[unroll]
		for ( uint slot = 0; slot < 4; slot++ ) {
			const float4 held = tapColor[slot];
			const float2 heldAux = tapAux[slot];
			// An edge the history holds without noise is not mixed: of its two sides the pixel takes its own. A
			// texel farther from the frame's value than EDGE_DEVIATIONS of its noise explain, and much farther
			// than the nearest texel, lies across the edge. A penumbra's lone sample has no texel near it: all stay.
			const float heldLuminance = dot( held.rgb, RT_LUMA );
			const float heldNoise = heldAux.x - heldLuminance * heldLuminance;
			const float deviations = EDGE_DEVIATIONS * EDGE_DEVIATIONS * max( heldNoise - 5e-4 * heldAux.x, 0.0 );
			const bool sure = held.a >= CUT_FRAMES && tapFar[slot] * tapFar[slot] > deviations;
			const bool across = !plain && sure && tapFar[slot] > max( EDGE_STEP, 2.0 * nearest );
			const float w = across ? 0.0 : tapWeight[slot];
			if ( w > 0.0 ) {
				if ( weight == 0.0 ) {
					first = held;
					firstNoise = heldNoise;
				}
				mean += w * ( held.rgb - first.rgb );
				frames += w * ( held.a - first.a );
				noise += w * ( heldNoise - firstNoise );
				least = min( least, heldAux.y );
				most = max( most, heldAux.y );
				weight += w;
			}
		}
		if ( weight > 0.0 && measure <= most * 1.25 + 1e-6 && measure >= least * 0.75 - 1e-6 ) {
			mean = first.rgb + mean / weight;
			frames = first.a + frames / weight;
			const float mixed = dot( mean, RT_LUMA );
			second = max( mixed * mixed + firstNoise + noise / weight, 0.0 );
			// A history outside what the neighbours hold now is of a shadow that moved on. Its own noise explains
			// CUT_SPREAD deviations between its mean over its frames and the mean of n neighbours: the more
			// of them agree, the less it may differ. A history of a few frames knows no noise yet.
			const float known = max( second - mixed * mixed - 5e-4 * second, 0.0 );
			const bool whole = plain || frames < CUT_FRAMES;
			const float spread = whole ? 1.0 : CUT_SPREAD * sqrt( known * ( 1.0 / inner + 1.0 / frames ) );
			const float wideSpread = whole ? 1.0 : CUT_SPREAD * sqrt( known * ( 1.0 / taken + 1.0 / frames ) );
			const float3 inside = clamp( mean, max( low - spread, wideLow - wideSpread ), min( high + spread, wideHigh + wideSpread ) );
			const float3 cut = abs( mean - inside );
			const float again = saturate( CUT_FRAMES * max( cut.r, max( cut.g, cut.b ) ) );
			if ( again > 0.0 ) {
				// As far as it is cut it is a new pixel: the frame's value, the variance its neighbours show
				// (none where they agree: a moved shadow without noise is not blurred), and frames enough
				// to go by that variance.
				const float around = max( squares / taken - ( sum / taken ) * ( sum / taken ), 0.0 );
				mean = lerp( inside, value, again );
				const float after = dot( mean, RT_LUMA );
				second = after * after + lerp( known, around, again );
				frames = max( frames * ( 1.0 - again ), CUT_FRAMES - 1.0 );
			}
		} else {
			mean = float3( 0.0, 0.0, 0.0 );
			frames = 0.0;
			second = 0.0;
		}
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
