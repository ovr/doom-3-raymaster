// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

// The exact sRGB curves of IEC 61966-2-1, without b3: the render programs and the backend's passes share them.
// Above 1 both continue the same formula; below 0 the linear segment.

static const float HALF_MAX = 65504.0;

float3 SRGBToLinear( float3 c ) {
	return select( c <= 0.04045, c / 12.92, pow( max( ( c + 0.055 ) / 1.055, 0.0 ), 2.4 ) );
}

float3 LinearToSRGB( float3 c ) {
	return select( c <= 0.0031308, c * 12.92, 1.055 * pow( max( c, 0.0 ), 1.0 / 2.4 ) - 0.055 );
}
