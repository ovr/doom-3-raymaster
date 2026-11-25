// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The thin G-buffer (R14 of RenderBackend.h), written by gbuffer.hlsl after the depth pass:

	  GB0  R16G16_SNORM         the world normal, octahedral
	  GB1  R8G8B8A8_UNORM_SRGB  rgb F0, the reflectance at normal incidence (linear; the view encodes it), a roughness
	  GB2  R16G16_FLOAT         the motion in pixels of the scene from the previous frame to this one, +y up

	A cleared texel is no surface: normal (0, 0), F0 0, no motion.
*/

float2 OctahedralWrap( float2 v ) {
	return ( 1.0 - abs( v.yx ) ) * select( v >= 0.0, float2( 1.0, 1.0 ), float2( -1.0, -1.0 ) );
}

float2 EncodeOctahedral( float3 n ) {
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	return ( n.z >= 0.0 ) ? n.xy : OctahedralWrap( n.xy );
}

float3 DecodeOctahedral( float2 e ) {
	float3 n = float3( e.x, e.y, 1.0 - abs( e.x ) - abs( e.y ) );
	const float t = saturate( -n.z );
	n.xy += select( n.xy >= 0.0, float2( -t, -t ), float2( t, t ) );
	return normalize( n );
}
