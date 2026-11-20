// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// One eye's image through the radial warp of the Rift's optics onto a square block of pixels (renderprogs/stereoWarp).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

Texture2D eyeMap : register( t0 );
SamplerState eyeSampler : register( s0 );

struct VSInput {
	float4 position : POSITION;
	float4 texcoord : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float4 texcoord : TEXCOORD0;		// 0 to 1 box
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
	const float screenWarpRange = 1.45;

	const float2 warpCenter = float2( 0.5, 0.5 );
	const float2 centeredTexcoord = input.texcoord.xy - warpCenter;

	const float radialLength = length( centeredTexcoord );
	const float2 radialDir = normalize( centeredTexcoord );

	// down into the 0 - PI/2 range
	const float range = screenWarpRange;
	const float scaledRadialLength = radialLength * range;
	const float tanScaled = tan( scaledRadialLength );

	const float rescaleValue = tan( 0.5 * range );

	// a radialLength of 0.5 rescales to 0.5, so the edges of the image are the edges of the warped image
	const float rescaled = tanScaled / rescaleValue;

	const float2 warped = warpCenter + float2( 0.5, 0.5 ) * radialDir * rescaled;

	return eyeMap.Sample( eyeSampler, warped );
}
