// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A blend light: the light's projected image times its falloff, times rpColor (renderprogs/blendLight).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpTexGen0S;
	float4 rpTexGen0T;
	float4 rpTexGen0Q;
	float4 rpTexGen1S;
};

cbuffer PixelParms : register( b1 ) {
	float4 rpColor;
};

Texture2D lightProjectionMap : register( t0 );
Texture2D lightFalloffMap : register( t1 );
SamplerState lightProjectionSampler : register( s0 );
SamplerState lightFalloffSampler : register( s1 );

struct VSInput {
	float4 position : POSITION;
};

struct VSOutput {
	float4 position : SV_Position;
	float4 lightProjection : TEXCOORD0;
	float2 lightFalloff : TEXCOORD1;
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

	output.lightProjection.x = dot4( input.position, rpTexGen0S );
	output.lightProjection.y = dot4( input.position, rpTexGen0T );
	output.lightProjection.z = 0.0;
	output.lightProjection.w = dot4( input.position, rpTexGen0Q );

	output.lightFalloff.x = dot4( input.position, rpTexGen1S );
	output.lightFalloff.y = 0.5;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	return SampleProjected( lightProjectionMap, lightProjectionSampler, input.lightProjection )
		* lightFalloffMap.Sample( lightFalloffSampler, input.lightFalloff ) * rpColor;
}
