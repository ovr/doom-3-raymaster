// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A texture times rpColor, through the texture matrix or the old-school texgen (renderprogs/texture).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpTextureMatrixS;
	float4 rpTextureMatrixT;
	float4 rpTexGen0S;
	float4 rpTexGen0T;
	float4 rpTexGen0Enabled;
};

cbuffer PixelParms : register( b1 ) {
	float4 rpColor;
};

TEXTURE2D( Texture2D, textureMap, 0 );
SAMPLER( textureSampler, 0 );

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

	[branch] if ( rpTexGen0Enabled.x > 0.0 ) {
		output.texcoord.x = dot4( input.position, rpTexGen0S );
		output.texcoord.y = dot4( input.position, rpTexGen0T );
	} else {
		output.texcoord.x = dot4( input.texcoord.xy, rpTextureMatrixS );
		output.texcoord.y = dot4( input.texcoord.xy, rpTextureMatrixT );
	}
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	return textureMap.Sample( textureSampler, input.texcoord ) * rpColor;
}
