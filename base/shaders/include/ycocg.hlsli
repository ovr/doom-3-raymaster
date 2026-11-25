// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

// The diffuse maps are YCoCg-DXT compressed: Co, Cg, the scale and Y in alpha. Without common.hlsli, so that the
// compute stages can read them too.
static const float4 matrixCoCg1YtoRGB1X = float4( 1.0, -1.0, 0.0, 1.0 );
static const float4 matrixCoCg1YtoRGB1Y = float4( 0.0, 1.0, -0.50196078, 1.0 );	// -0.5 * 256.0 / 255.0
static const float4 matrixCoCg1YtoRGB1Z = float4( -1.0, -1.0, 1.00392156, 1.0 );	// +1.0 * 256.0 / 255.0

float3 ConvertYCoCgToRGB( float4 YCoCg ) {
	float3 rgbColor;
	YCoCg.z = ( YCoCg.z * 31.875 ) + 1.0;	// z = z * 255.0 / 8.0 + 1.0
	YCoCg.z = 1.0 / YCoCg.z;
	YCoCg.xy *= YCoCg.z;
	rgbColor.x = dot( YCoCg, matrixCoCg1YtoRGB1X );
	rgbColor.y = dot( YCoCg, matrixCoCg1YtoRGB1Y );
	rgbColor.z = dot( YCoCg, matrixCoCg1YtoRGB1Z );
	return rgbColor;
}
