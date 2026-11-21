// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// Grey shading from the depth alone: the normal from the screen-space derivatives, lit from the upper right (renderprogs/simpleshade).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

struct VSInput {
	float4 position : POSITION;
};

struct VSOutput {
	float4 position : SV_Position;
	float4 clipPosition : TEXCOORD0;	// GL's clip space
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
	output.clipPosition = glPosition;
	return output;
}

float2 ScreenPosToTexcoord( float2 pos, float4 biasScale ) {
	return ( pos * biasScale.zw + biasScale.xy );
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float renderWidth = 1280.0;
	const float renderHeight = 720.0;
	const float4 positionToViewTexture = float4( 0.5 / renderWidth, 0.5 / renderHeight, 1.0 / renderWidth, 1.0 / renderHeight );

	const float4 fragCoord = FragCoord( input.position );
	const float interpolatedZOverW = ( 1.0 - ( input.clipPosition.z / input.clipPosition.w ) );

	float3 pos;
	pos.z = 1.0 / interpolatedZOverW;
	pos.xy = pos.z * ( 2.0 * ScreenPosToTexcoord( fragCoord.xy, positionToViewTexture ) - 1.0 );

	const float3 normal = normalize( cross( ddy( pos ), ddx( pos ) ) );

	// the light is above and to the right in the eye plane
	const float3 toLight = normalize( float3( 1.0, 1.0, 0.0 ) - pos );

	float4 color;
	color.xyz = (float3)( dot3( normal, toLight ) * 0.75 );
	color.w = 1.0;
	return SceneOutput( color );
}
