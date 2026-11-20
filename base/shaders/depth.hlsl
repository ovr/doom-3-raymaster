// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The depth pass: only the depth counts, the colour is black (renderprogs/depth).
// SKINNED=1 is depth_skinned: the joints in COLOR0 and the weights in COLOR1.

#include "include/common.hlsli"
#if SKINNED
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
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
	const float4 modelPosition = SkinPosition( SkinningMatrix( input.color, input.color2 ), input.position );
#else
	const float4 modelPosition = input.position;
#endif

	float4 glPosition;
	glPosition.x = dot4( modelPosition, rpMVPmatrixX );
	glPosition.y = dot4( modelPosition, rpMVPmatrixY );
	glPosition.z = dot4( modelPosition, rpMVPmatrixZ );
	glPosition.w = dot4( modelPosition, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	return float4( 0.0, 0.0, 0.0, 1.0 );
}
