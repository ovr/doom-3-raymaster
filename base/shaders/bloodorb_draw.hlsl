// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The blood orb on the screen: the accumulated orb over the current render where the mask lets it (renderprogs/bloodorb_draw).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

Texture2D accumMap : register( t0 );
Texture2D currentRenderMap : register( t1 );
Texture2D maskMap : register( t2 );
SamplerState accumSampler : register( s0 );
SamplerState currentRenderSampler : register( s1 );
SamplerState maskSampler : register( s2 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord0 : TEXCOORD0;
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

	output.texcoord0 = input.texcoord;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float4 accumSample = accumMap.Sample( accumSampler, input.texcoord0 );
	const float4 currentRenderSample = currentRenderMap.Sample( currentRenderSampler, input.texcoord0 );
	const float4 maskSample = maskMap.Sample( maskSampler, input.texcoord0 );

	return lerp( accumSample, currentRenderSample, maskSample.a );
}
