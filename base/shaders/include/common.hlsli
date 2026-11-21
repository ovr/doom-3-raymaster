// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "bindless.hlsli"
#include "colorspace.hlsli"

/*
	What every render program shares. The shared code still feeds the conventions of id's renderprogs:
	matrices and clip space are GL's, vertex colours are UNORM bytes, the parms are float4 named like
	renderParm_t. A port is one to one from base/renderprogs, the same math in the same order, and:

	  - the clip position is precise: a depth-equal pass must land on the same depth as the depth pass;
	  - COLOR0/COLOR1 outputs are saturated, as GL clamps them; TEXCOORDn outputs are not;
	  - gl_FragCoord is FragCoord( SV_Position ): the same xyz, w = 1 / w.
*/

// GL's clip space (z in -w..w) to Direct3D's (z in 0..w), y flipped for the top-down targets.
float4 GLClipToD3D( float4 p ) {
	return float4( p.x, -p.y, ( p.z + p.w ) * 0.5, p.w );
}

float4 FragCoord( float4 svPosition ) {
	return float4( svPosition.xyz, 1.0 / svPosition.w );
}

// id's dot helpers: a float2 texture coordinate is (s, t, 0, 1) against a float4 row.
float dot2( float2 a, float2 b ) { return dot( a, b ); }
float dot3( float3 a, float3 b ) { return dot( a, b ); }
float dot3( float3 a, float4 b ) { return dot( a, b.xyz ); }
float dot3( float4 a, float3 b ) { return dot( a.xyz, b ); }
float dot3( float4 a, float4 b ) { return dot( a.xyz, b.xyz ); }
float dot4( float4 a, float4 b ) { return dot( a, b ); }
float dot4( float2 a, float4 b ) { return dot( float4( a, 0, 1 ), b ); }

// tex2Dproj of the renderprogs: the coordinates divided by their last component.
float4 SampleProjected( Texture2D map, SamplerState mapSampler, float4 coords ) {
	return map.Sample( mapSampler, coords.xy / coords.w );
}

// The diffuse maps are YCoCg-DXT compressed.
static const float4 matrixCoCg1YtoRGB1X = float4( 1.0, -1.0, 0.0, 1.0 );
static const float4 matrixCoCg1YtoRGB1Y = float4( 0.0, 1.0, -0.50196078, 1.0 );	// -0.5 * 256.0 / 255.0
static const float4 matrixCoCg1YtoRGB1Z = float4( -1.0, -1.0, 1.00392156, 1.0 );	// +1.0 * 256.0 / 255.0

float3 ConvertYCoCgToRGB( float4 YCoCg ) {
	float3 rgbColor;
	YCoCg.z = ( YCoCg.z * 31.875 ) + 1.0;	// z = z * 255.0 / 8.0 + 1.0
	YCoCg.z = 1.0 / YCoCg.z;
	YCoCg.xy *= YCoCg.z;
	rgbColor.x = dot4( YCoCg, matrixCoCg1YtoRGB1X );
	rgbColor.y = dot4( YCoCg, matrixCoCg1YtoRGB1Y );
	rgbColor.z = dot4( YCoCg, matrixCoCg1YtoRGB1Z );
	return rgbColor;
}
