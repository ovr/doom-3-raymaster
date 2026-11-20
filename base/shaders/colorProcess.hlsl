// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A texture blended by a fraction towards its grey value times a target hue (renderprogs/colorProcess).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpUser0;		// the fraction
	float4 rpUser1;		// the target hue
};

Texture2D sourceMap : register( t0 );
SamplerState sourceSampler : register( s0 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float4 color : COLOR0;
	float3 texcoord0 : TEXCOORD0;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
	float4 glPosition;
	glPosition.x = dot4( input.position, rpMVPmatrixX );
	glPosition.y = dot4( input.position, rpMVPmatrixY );
	glPosition.z = dot4( input.position, rpMVPmatrixZ );
	glPosition.w = dot4( input.position, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;

	output.color = saturate( rpUser1 );

	output.texcoord0.x = input.texcoord.x;
	output.texcoord0.y = 1.0 - input.texcoord.y;

	output.texcoord0.z = rpUser0.x;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float4 src = sourceMap.Sample( sourceSampler, input.texcoord0.xy );
	const float4 target = input.color * dot3( float3( 0.333, 0.333, 0.333 ), src );
	return lerp( src, target, input.texcoord0.z );
}
