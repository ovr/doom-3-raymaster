// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A projected texture times the vertex colour: texgen planes, then the texture matrix (renderprogs/texture_color_texgen).

#include "include/common.hlsli"

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
	float4 rpTexGen0Q;
};

Texture2D textureMap : register( t0 );
SamplerState textureSampler : register( s0 );

struct VSInput {
	float4 position : POSITION;
	float4 color : COLOR0;
};

struct VSOutput {
	float4 position : SV_Position;
	float4 texcoord : TEXCOORD0;
	float4 color : COLOR0;
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

	float4 texGen;
	texGen.x = dot4( input.position, rpTexGen0S );
	texGen.y = dot4( input.position, rpTexGen0T );
	texGen.z = 0.0;
	texGen.w = dot4( input.position, rpTexGen0Q );

	output.texcoord.x = dot4( texGen, rpTextureMatrixS );
	output.texcoord.y = dot4( texGen, rpTextureMatrixT );
	output.texcoord.zw = texGen.zw;

	const float4 vertexColor = ( input.color * rpVertexColorModulate ) + rpVertexColorAdd;
	output.color = saturate( vertexColor * rpColor );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	// Always projective, so texgen materials need no program of their own: plain coordinates have w = 1.
	const float4 texSample = SampleProjected( textureMap, textureSampler, input.texcoord );
	return texSample * input.color;
}
