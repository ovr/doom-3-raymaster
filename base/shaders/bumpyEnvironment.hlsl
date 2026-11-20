// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A cube map reflected about the bump-mapped normal in world space, times rpColor (renderprogs/bumpyenvironment).
// SKINNED=1 is bumpyEnvironment_skinned: the joints in COLOR0 and COLOR1 move the position and the tangent frame.

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
	float4 rpModelMatrixX;
	float4 rpModelMatrixY;
	float4 rpModelMatrixZ;
};

TEXTURECUBE( TextureCube, environmentMap, 0 );
TEXTURE2D( Texture2D, bumpMap, 1 );
SAMPLER( environmentSampler, 0 );
SAMPLER( bumpSampler, 1 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 normal : NORMAL;
	float4 tangent : TANGENT;
#if SKINNED
	float4 color : COLOR0;
	float4 color2 : COLOR1;
#endif
};

// The tangent-to-world rows: the world x, y and z of the tangent, the binormal and the normal.
struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
	float3 toEye : TEXCOORD1;			// world space
	float3 tangentToWorldX : TEXCOORD2;
	float3 tangentToWorldY : TEXCOORD3;
	float3 tangentToWorldZ : TEXCOORD4;
	float4 color : COLOR0;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
	const float4 vNormal = input.normal * 2.0 - 1.0;
	const float4 vTangent = input.tangent * 2.0 - 1.0;
	const float3 vBinormal = cross( vNormal.xyz, vTangent.xyz ) * vTangent.w;
#if SKINNED
	const SkinMatrix skin = SkinningMatrix( input.color, input.color2 );
	const float3 normal = SkinDirection( skin, vNormal.xyz );
	const float3 tangent = SkinDirection( skin, vTangent.xyz );
	const float3 binormal = SkinDirection( skin, vBinormal );
	const float4 modelPosition = SkinPosition( skin, input.position );
#else
	const float3 normal = vNormal.xyz;
	const float3 tangent = vTangent.xyz;
	const float3 binormal = vBinormal;
	const float4 modelPosition = input.position;
#endif

	float4 glPosition;
	glPosition.x = dot4( modelPosition, rpMVPmatrixX );
	glPosition.y = dot4( modelPosition, rpMVPmatrixY );
	glPosition.z = dot4( modelPosition, rpMVPmatrixZ );
	glPosition.w = dot4( modelPosition, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;

	output.texcoord = input.texcoord.xy;

	const float4 toEye = rpLocalViewOrigin - modelPosition;
	output.toEye = float3( dot3( toEye, rpModelMatrixX ), dot3( toEye, rpModelMatrixY ), dot3( toEye, rpModelMatrixZ ) );

	output.tangentToWorldX = float3( dot3( tangent, rpModelMatrixX ), dot3( binormal, rpModelMatrixX ), dot3( normal, rpModelMatrixX ) );
	output.tangentToWorldY = float3( dot3( tangent, rpModelMatrixY ), dot3( binormal, rpModelMatrixY ), dot3( normal, rpModelMatrixY ) );
	output.tangentToWorldZ = float3( dot3( tangent, rpModelMatrixZ ), dot3( binormal, rpModelMatrixZ ), dot3( normal, rpModelMatrixZ ) );

	output.color = saturate( rpColor );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float4 bump = bumpMap.Sample( bumpSampler, input.texcoord ) * 2.0 - 1.0;
	float3 localNormal = float3( bump.wy, 0.0 );
	localNormal.z = sqrt( 1.0 - dot3( localNormal, localNormal ) );

	float3 globalNormal;
	globalNormal.x = dot3( localNormal, input.tangentToWorldX );
	globalNormal.y = dot3( localNormal, input.tangentToWorldY );
	globalNormal.z = dot3( localNormal, input.tangentToWorldZ );

	const float3 globalEye = normalize( input.toEye );

	float3 reflectionVector = globalNormal * dot3( globalEye, globalNormal );
	reflectionVector = ( reflectionVector * 2.0 ) - globalEye;

	const float4 envMap = environmentMap.Sample( environmentSampler, reflectionVector );

	return float4( envMap.xyz, 1.0 ) * input.color;
}
