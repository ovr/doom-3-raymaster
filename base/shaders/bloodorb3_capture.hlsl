// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The blood orb's third capture: three turned accumulation samples over the tinted current render under the mask (renderprogs/bloodorb3_capture).

#include "include/common.hlsli"
#include "include/bloodorb.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpUser0;		// the center scale
	float4 rpUser1;		// the rotations
	float4 rpUser2;		// the color factor
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
	float2 texcoord1 : TEXCOORD1;
	float2 texcoord2 : TEXCOORD2;
	float2 texcoord3 : TEXCOORD3;
	float2 texcoord4 : TEXCOORD4;
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

	// the three accumulation samples
	const float4 centerScaleTex = rpUser0;
	const float4 rotateTex = rpUser1;
	const float2 tc0 = CenterScale( input.texcoord, centerScaleTex.xy );
	output.texcoord0 = Rotate2D( tc0, rotateTex.xy );

	output.texcoord1 = Rotate2D( tc0, float2( rotateTex.z, -rotateTex.w ) );

	output.texcoord2 = Rotate2D( tc0, rotateTex.zw );

	// the current render and the mask
	output.texcoord3 = input.texcoord;

	const float4 colorFactor = rpUser2;
	output.texcoord4 = colorFactor.xx;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float colorFactor = input.texcoord4.x;

	const float4 color0 = float4( 1.0 - colorFactor, 1.0 - colorFactor, 1.0, 1.0 );
	const float4 color1 = float4( 1.0, 0.95 - colorFactor, 0.95, 0.5 );
	const float4 color2 = float4( 0.015, 0.015, 0.015, 0.01 );

	const float4 accumSample0 = accumMap.Sample( accumSampler, input.texcoord0 ) * color0;
	const float4 accumSample1 = accumMap.Sample( accumSampler, input.texcoord1 ) * color1;
	const float4 accumSample2 = accumMap.Sample( accumSampler, input.texcoord2 ) * color2;
	const float4 maskSample = maskMap.Sample( maskSampler, input.texcoord3 );

	const float4 tint = float4( 0.8, 0.5, 0.5, 1 );
	const float4 currentRenderSample = currentRenderMap.Sample( currentRenderSampler, input.texcoord3 ) * tint;

	// the first two accumulation samples blended, the third added
	float4 accumColor = lerp( accumSample0, accumSample1, 0.5 );
	accumColor += accumSample2;

	accumColor = lerp( accumColor, currentRenderSample, maskSample.a );
	return accumColor;
}
