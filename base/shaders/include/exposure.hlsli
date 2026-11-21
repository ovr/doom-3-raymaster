// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

// The eye adaptation's buffers (d3d12_Hdr.h, d3d12Exposure). The histogram holds EXPOSURE_BINS counts of EV100;
// EyeAdaptation holds float4( exposure, measured EV100, adapted EV100, target EV100 ), then the last counts.

#define EXPOSURE_BINS 64
#define EYE_HISTOGRAM 16			// the byte offset of the counts in EyeAdaptation
#define EXPOSURE_WEIGHT_ONE 32.0	// a pixel's full weight in the histogram
#define EXPOSURE_TARGET_STEPS 16.0	// the target moves in steps of 1/16 EV

static const float3 EXPOSURE_LUMA = float3( 0.2126, 0.7152, 0.0722 );

// EV100 of a luminance with the reflected-light meter's constant 12.5: log2( L * 100 / 12.5 ).
float LuminanceToEV100( float luminance ) {
	return log2( luminance ) + 3.0;
}
