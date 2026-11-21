// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The eye adaptation of the HDR resolve, one thread after the histogram:

	  measured   the mean EV100 of the histogram's weight between lowPercent and highPercent (the last one when empty)
	  target     measured in steps of 1/16 EV, clamped to minEV100 .. maxEV100: a lone wrong 2 x 2 block (a driver flake)
	             moves the mean by about 1e-5 EV, so it almost never reaches the adapted EV100 and two runs agree
	  adapted    toward the target by 1 - 2^( -seconds * speed ): speedUp toward a brighter frame, speedDown toward a
	             darker one; at once on a reset
	  exposure   auto: 2^( compensation + bias - adapted ) / 1.2 (the saturation-based exposure of ISO 100);
	             manual: the CPU's, 2^compensation

	It copies the counts into EyeAdaptation (include/exposure.hlsli) and empties the histogram for the next frame.
*/

#include "../include/exposure.hlsli"

cbuffer AdaptParms : register( b0 ) {
	uint histogramUav;
	uint eyeUav;
	float histogramMinEV;
	float binWidth;			// EV per bin
	float lowPercent;		// 0..1 of the weight
	float highPercent;
	float minEV100;
	float maxEV100;
	float speedUp;			// per second
	float speedDown;
	float seconds;
	uint reset;
	uint autoExposure;
	float compensation;		// EV, auto exposure
	float bias;				// EV, auto exposure
	float manualExposure;
};

[numthreads( 1, 1, 1 )]
void CSMain() {
	RWByteAddressBuffer histogram = ResourceDescriptorHeap[histogramUav];
	RWByteAddressBuffer eye = ResourceDescriptorHeap[eyeUav];
	uint counts[EXPOSURE_BINS];
	uint total = 0;
	for ( uint i = 0; i < EXPOSURE_BINS; i++ ) {
		counts[i] = histogram.Load( i * 4 );
		histogram.Store( i * 4, 0 );
		eye.Store( EYE_HISTOGRAM + i * 4, counts[i] );
		total += counts[i];
	}
	const float4 last = asfloat( eye.Load4( 0 ) );
	float measured = last.y;
	if ( total != 0 ) {
		const float low = float( total ) * lowPercent;
		const float high = float( total ) * highPercent;
		float below = 0.0;
		float sum = 0.0;
		float weight = 0.0;
		for ( uint b = 0; b < EXPOSURE_BINS; b++ ) {
			const float count = float( counts[b] );
			const float inside = max( 0.0, min( below + count, high ) - max( below, low ) );
			sum += inside * ( histogramMinEV + ( float( b ) + 0.5 ) * binWidth );
			weight += inside;
			below += count;
		}
		measured = ( weight > 0.0 ) ? sum / weight : measured;
	}
	const float target = clamp( round( measured * EXPOSURE_TARGET_STEPS ) / EXPOSURE_TARGET_STEPS, minEV100, maxEV100 );
	float adapted = last.z;
	if ( reset != 0 || !( abs( adapted ) < 1000.0 ) ) {	// a NaN fails the test too
		adapted = target;
	} else {
		const float speed = ( target > adapted ) ? speedUp : speedDown;
		adapted += ( target - adapted ) * ( 1.0 - exp2( -seconds * speed ) );
	}
	const float exposure = ( autoExposure != 0 ) ? exp2( compensation + bias - adapted ) / 1.2 : manualExposure;
	eye.Store4( 0, asuint( float4( exposure, measured, adapted, target ) ) );
}
