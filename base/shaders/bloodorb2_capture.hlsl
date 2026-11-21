// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The blood orb's second capture: the turned, tinted accumulation over the tinted current render under the mask (renderprogs/bloodorb2_capture).

#include "include/common.hlsli"
#include "include/bloodorb.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpUser0;		// the center scale of the accumulation
	float4 rpUser1;		// the rotation of the accumulation
	float4 rpUser2;		// the center scale of the mask
};

TEXTURE2D( Texture2D, accumMap, 0 );
TEXTURE2D( Texture2D, currentRenderMap, 1 );
TEXTURE2D( Texture2D, maskMap, 2 );
SAMPLER( accumSampler, 0 );
SAMPLER( currentRenderSampler, 1 );
SAMPLER( maskSampler, 2 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord0 : TEXCOORD0;
	float2 texcoord1 : TEXCOORD1;
	float2 texcoord2 : TEXCOORD2;
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

	const float4 centerScaleTex0 = rpUser0;
	const float4 rotateTex0 = rpUser1;
	const float4 centerScaleTex1 = rpUser2;

	const float2 tc0 = CenterScale( input.texcoord, centerScaleTex0.xy );
	output.texcoord0 = Rotate2D( tc0, rotateTex0.xy );

	output.texcoord1 = CenterScale( input.texcoord, centerScaleTex1.xy );

	output.texcoord2 = input.texcoord;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float4 redTint = float4( 1, 0.98, 0.98, 1 );
	const float4 accumSample = SceneColor( accumMap.Sample( accumSampler, input.texcoord0 ), 0 ) * redTint;

	const float4 maskSample = maskMap.Sample( maskSampler, input.texcoord1 );

	const float4 tint = float4( 1.0, 0.8, 0.8, 1 );
	const float4 currentRenderSample = SceneColor( currentRenderMap.Sample( currentRenderSampler, input.texcoord2 ), 1 ) * tint;

	return SceneOutput( lerp( accumSample, currentRenderSample, maskSample.a ) );
}
