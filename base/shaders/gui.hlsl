// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The GUIs: a texture times the vertex colour plus COLOR1 as a signed add, premultiplied (renderprogs/gui).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

Texture2D guiMap : register( t0 );
SamplerState guiSampler : register( s0 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
	float4 color2 : COLOR1;
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
	float4 add : TEXCOORD1;
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
	output.texcoord = input.texcoord;
	output.add = ( input.color2 * 2 ) - 1;
	output.color = saturate( input.color );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float4 color = ( guiMap.Sample( guiSampler, input.texcoord ) * input.color ) + input.add;
	return float4( color.xyz * color.w, color.w );
}
