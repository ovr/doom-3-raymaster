// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A sky cube map seen from the view origin through the wobble rotation, times the vertex colour (renderprogs/wobblesky).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpLocalViewOrigin;
	float4 rpVertexColorModulate;
	float4 rpVertexColorAdd;
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpWobbleSkyX;
	float4 rpWobbleSkyY;
	float4 rpWobbleSkyZ;
};

TEXTURECUBE( TextureCube, skyMap, 0 );
SAMPLER( skySampler, 0 );

struct VSInput {
	float4 position : POSITION;
	float4 color : COLOR0;
};

struct VSOutput {
	float4 position : SV_Position;
	float3 direction : TEXCOORD0;
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

	const float3 fromView = input.position.xyz - rpLocalViewOrigin.xyz;
	output.direction.x = dot3( fromView, rpWobbleSkyX );
	output.direction.y = dot3( fromView, rpWobbleSkyY );
	output.direction.z = dot3( fromView, rpWobbleSkyZ );

	output.color = saturate( ( input.color * rpVertexColorModulate ) + rpVertexColorAdd );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	return skyMap.Sample( skySampler, input.direction ) * input.color;
}
