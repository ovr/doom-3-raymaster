// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The view blurred along each pixel's motion since the previous frame, found from the depth (renderprogs/motionBlur).

#include "include/common.hlsli"

// rpMVPmatrix* is the previous frame's MVP times the inverse of this frame's; rpOverbright.x the sample count.
cbuffer PixelParms : register( b1 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpProjectionMatrixZ;
	float4 rpOverbright;
};

Texture2D viewColorMap : register( t0 );
Texture2D viewDepthMap : register( t1 );
SamplerState viewColorSampler : register( s0 );
SamplerState viewDepthSampler : register( s1 );

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
	const float4 glPosition = input.position;
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;
	output.texcoord = input.texcoord;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	// don't blur the hands, which were drawn with alpha = 0
	if ( viewColorMap.Sample( viewColorSampler, input.texcoord ).w == 0.0 ) {
		discard;
	}

	// clip space from the depth buffer and the screen position
	const float windowZ = viewDepthMap.Sample( viewDepthSampler, input.texcoord ).x;
	const float3 ndc = float3( input.texcoord * 2.0 - 1.0, windowZ * 2.0 - 1.0 );
	const float clipW = -rpProjectionMatrixZ.w / ( -rpProjectionMatrixZ.z - ndc.z );

	const float4 clip = float4( ndc * clipW, clipW );

	// from this frame's clip space to the previous frame's
	float4 reClip;
	reClip.x = dot( rpMVPmatrixX, clip );
	reClip.y = dot( rpMVPmatrixY, clip );
	reClip.z = dot( rpMVPmatrixZ, clip );
	reClip.w = dot( rpMVPmatrixW, clip );

	float2 prevTexCoord;
	prevTexCoord.x = ( reClip.x / reClip.w ) * 0.5 + 0.5;
	prevTexCoord.y = ( reClip.y / reClip.w ) * 0.5 + 0.5;

	// samples along the line from prevTexCoord to the texcoord
	const float2 delta = ( input.texcoord - prevTexCoord );

	float3 sum = float3( 0.0, 0.0, 0.0 );
	float goodSamples = 0;
	const float samples = rpOverbright.x;

	for ( float i = 0; i < samples; i = i + 1 ) {
		const float2 pos = input.texcoord + delta * ( ( i / ( samples - 1 ) ) - 0.5 );
		const float4 color = viewColorMap.Sample( viewColorSampler, pos );
		// only the values that are not part of the weapon
		sum += color.xyz * color.w;
		goodSamples += color.w;
	}
	const float invScale = 1.0 / goodSamples;

	return float4( sum * invScale, 1.0 );
}
