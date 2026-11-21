// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The shadow volumes made visible in rpColor (r_showShadows); the vertex stage is shadow's (renderprogs/shadowDebug).
// SKINNED=1 is shadowDebug_skinned: the joints in COLOR0 and the weights in COLOR1; the skinning keeps the vertex's w.

#include "include/common.hlsli"
#if SKINNED
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpLocalLightOrigin;
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

cbuffer PixelParms : register( b1 ) {
	float4 rpColor;
};

struct VSInput {
	float4 position : POSITION;
#if SKINNED
	float4 color : COLOR0;
	float4 color2 : COLOR1;
#endif
};

struct VSOutput {
	float4 position : SV_Position;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
#if SKINNED
	float4 vertexPosition = input.position;
	vertexPosition.w = 1.0;
	float4 modelPosition = SkinPosition( SkinningMatrix( input.color, input.color2 ), vertexPosition );
	modelPosition.w = input.position.w;
#else
	const float4 modelPosition = input.position;
#endif

	// w = 0 vertices go to infinity away from the light
	float4 volumePosition = modelPosition - rpLocalLightOrigin;
	volumePosition = ( volumePosition.wwww * rpLocalLightOrigin ) + volumePosition;

	float4 glPosition;
	glPosition.x = dot4( volumePosition, rpMVPmatrixX );
	glPosition.y = dot4( volumePosition, rpMVPmatrixY );
	glPosition.z = dot4( volumePosition, rpMVPmatrixZ );
	glPosition.w = dot4( volumePosition, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	return SceneOutput( rpColor );
}
