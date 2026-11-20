// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The environment suit's view: the screen warped by a bump map times the deform magnitude (renderprogs/enviroSuit).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpUser1;		// the deform magnitude
};

Texture2D currentRenderMap : register( t0 );
Texture2D bumpMap : register( t1 );
SamplerState currentRenderSampler : register( s0 );
SamplerState bumpSampler : register( s1 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
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

	output.texcoord = input.texcoord.xy;

	const float4 deformMagnitude = rpUser1;
	output.color = saturate( deformMagnitude );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	float2 screenTexCoord = input.texcoord;

	const float4 warpFactor = 1.0 - ( bumpMap.Sample( bumpSampler, screenTexCoord.xy ) * input.color );
	screenTexCoord -= float2( 0.5, 0.5 );
	screenTexCoord *= warpFactor.xy;
	screenTexCoord += float2( 0.5, 0.5 );

	return currentRenderMap.Sample( currentRenderSampler, screenTexCoord );
}
