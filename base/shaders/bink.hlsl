// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A Bink video frame: its Y, Cr and Cb planes to RGB, times rpColor (renderprogs/bink).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

cbuffer PixelParms : register( b1 ) {
	float4 rpColor;
};

Texture2D yMap : register( t0 );
Texture2D crMap : register( t1 );
Texture2D cbMap : register( t2 );
SamplerState ySampler : register( s0 );
SamplerState crSampler : register( s1 );
SamplerState cbSampler : register( s2 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
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
	output.texcoord = input.texcoord;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float3 crc = float3( 1.595794678, -0.813476563, 0 );
	const float3 crb = float3( 0, -0.391448975, 2.017822266 );
	const float3 adj = float3( -0.87065506, 0.529705048, -1.081668854 );
	const float3 YScalar = float3( 1.164123535, 1.164123535, 1.164123535 );

	const float Y = yMap.Sample( ySampler, input.texcoord.xy ).x;
	const float Cr = crMap.Sample( crSampler, input.texcoord.xy ).x;
	const float Cb = cbMap.Sample( cbSampler, input.texcoord.xy ).x;

	float3 p = ( YScalar * Y );
	p += ( crc * Cr ) + ( crb * Cb ) + adj;

	float4 color;
	color.xyz = p;
	color.w = 1.0;
	color *= rpColor;
	return color;
}
