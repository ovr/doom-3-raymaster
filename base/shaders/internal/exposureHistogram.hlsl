// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The histogram of the eye adaptation: the viewport's luminance (the scene's divided by its preExposure), one
	value per 2 x 2 pixels, into EXPOSURE_BINS bins of EV100 from minEV on. Darker than the range (black, the
	shadows) does not count, brighter counts in the last bin. A value weighs lerp( 1, exp( -|d|^2 ), meteringMask ),
	d from -1 at the viewport's edges to 1, in whole 32ths: integer sums do not depend on the order of the atomics.
*/

#include "../include/exposure.hlsli"

cbuffer HistogramParms : register( b0 ) {
	uint sceneSrv;			// RGBA16F, one sample
	uint histogramUav;		// RWByteAddressBuffer of EXPOSURE_BINS counts, empty before the first group
	uint x0;				// the viewport
	uint y0;
	uint width;
	uint height;
	float invPreExposure;
	float meteringMask;
	float minEV;
	float binsPerEV;
};

groupshared uint bins[EXPOSURE_BINS];

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex ) {
	if ( index < EXPOSURE_BINS ) {
		bins[index] = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	const uint2 blocks = ( uint2( width, height ) + 1 ) / 2;
	if ( all( id.xy < blocks ) ) {
		Texture2D<float4> scene = ResourceDescriptorHeap[sceneSrv];
		const int2 origin = int2( x0, y0 ) + int2( id.xy * 2 );
		const int2 last = int2( x0 + width, y0 + height ) - 1;
		float3 sum = 0.0;
		[unroll] for ( int i = 0; i < 4; i++ ) {
			sum += scene.Load( int3( min( origin + int2( i & 1, i >> 1 ), last ), 0 ) ).rgb;
		}
		const float ev = LuminanceToEV100( dot( sum, EXPOSURE_LUMA ) * 0.25 * invPreExposure );
		if ( ev >= minEV ) {		// false for 0, negative and NaN
			const uint bin = (uint)min( ( ev - minEV ) * binsPerEV, EXPOSURE_BINS - 1.0 );
			const float2 d = ( float2( id.xy ) + 0.5 ) / float2( blocks ) * 2.0 - 1.0;
			const float weight = lerp( 1.0, exp( -dot( d, d ) ), meteringMask );
			InterlockedAdd( bins[bin], (uint)( weight * EXPOSURE_WEIGHT_ONE + 0.5 ) );
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if ( index < EXPOSURE_BINS && bins[index] != 0 ) {
		RWByteAddressBuffer histogram = ResourceDescriptorHeap[histogramUav];
		histogram.InterlockedAdd( index * 4, bins[index] );
	}
}
