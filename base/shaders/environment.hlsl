// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A cube map reflected about the vertex normal, times rpColor (renderprogs/environment).
// SKINNED=1 is environment_skinned: the joints in COLOR0 and COLOR1 move the position and the normal.

#include "include/common.hlsli"
#if SKINNED
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpLocalViewOrigin;
	float4 rpColor;
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

TextureCube environmentMap : register( t0 );
SamplerState environmentSampler : register( s0 );

struct VSInput {
	float4 position : POSITION;
	float4 normal : NORMAL;
#if SKINNED
	float4 color : COLOR0;
	float4 color2 : COLOR1;
#endif
};

struct VSOutput {
	float4 position : SV_Position;
	float3 toEye : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float4 color : COLOR0;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
	const float4 vNormal = input.normal * 2.0 - 1.0;
#if SKINNED
	const SkinMatrix skin = SkinningMatrix( input.color, input.color2 );
	const float3 normal = SkinDirection( skin, vNormal.xyz );
	const float4 modelPosition = SkinPosition( skin, input.position );
#else
	const float3 normal = vNormal.xyz;
	const float4 modelPosition = input.position;
#endif

	float4 glPosition;
	glPosition.x = dot4( modelPosition, rpMVPmatrixX );
	glPosition.y = dot4( modelPosition, rpMVPmatrixY );
	glPosition.z = dot4( modelPosition, rpMVPmatrixZ );
	glPosition.w = dot4( modelPosition, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;

	const float4 toEye = rpLocalViewOrigin - modelPosition;
	output.toEye = toEye.xyz;
	output.normal = normal;

	output.color = saturate( rpColor );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float3 globalNormal = normalize( input.normal );
	const float3 globalEye = normalize( input.toEye );

	float3 reflectionVector = (float3)dot3( globalEye, globalNormal );
	reflectionVector *= globalNormal;
	reflectionVector = ( reflectionVector * 2.0 ) - globalEye;

	const float4 envMap = environmentMap.Sample( environmentSampler, reflectionVector );

	return float4( envMap.xyz, 1.0 ) * input.color;
}
