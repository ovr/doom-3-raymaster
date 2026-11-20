// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The two eyes on alternate rows of the screen (renderprogs/stereoInterlace).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

Texture2D leftEyeMap : register( t0 );
Texture2D rightEyeMap : register( t1 );
SamplerState leftEyeSampler : register( s0 );
SamplerState rightEyeSampler : register( s1 );

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
	// the texcoords run from 0 to 1 across the entire screen
	if ( frac( FragCoord( input.position ).y * 0.5 ) < 0.5 ) {
		return leftEyeMap.Sample( leftEyeSampler, input.texcoord );
	} else {
		return rightEyeMap.Sample( rightEyeSampler, input.texcoord );
	}
}
