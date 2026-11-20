// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A texture times the vertex colour, alpha-tested, through the texture matrix or the texgen (renderprogs/texture_color).
// SKINNED=1 is texture_color_skinned: the joints in COLOR0, which is still the vertex colour, and the weights in COLOR1.

#include "include/common.hlsli"
#if SKINNED
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpVertexColorModulate;
	float4 rpVertexColorAdd;
	float4 rpColor;
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
	float4 rpAlphaTest;
};

Texture2D textureMap : register( t0 );
SamplerState textureSampler : register( s0 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
#if SKINNED
	float4 color2 : COLOR1;
#endif
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
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

	[branch] if ( rpTexGen0Enabled.x > 0.0 ) {
		output.texcoord.x = dot4( modelPosition, rpTexGen0S );
		output.texcoord.y = dot4( modelPosition, rpTexGen0T );
	} else {
		output.texcoord.x = dot4( input.texcoord.xy, rpTextureMatrixS );
		output.texcoord.y = dot4( input.texcoord.xy, rpTextureMatrixT );
	}

	const float4 vertexColor = ( input.color * rpVertexColorModulate ) + rpVertexColorAdd;
	output.color = saturate( vertexColor * rpColor );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float4 color = textureMap.Sample( textureSampler, input.texcoord ) * input.color;
	clip( color.a - rpAlphaTest.x );
	return color;
}
