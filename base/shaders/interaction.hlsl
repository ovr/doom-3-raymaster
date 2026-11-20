// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// One light on a surface: bump, diffuse and specular maps, the light's projection and falloff (renderprogs/interaction).
// SKINNED=1 is interaction_skinned: the joints in COLOR0 and COLOR1, so no vertex colour.

#include "include/common.hlsli"
#if SKINNED
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpLocalLightOrigin;
	float4 rpLocalViewOrigin;
	float4 rpLightProjectionS;
	float4 rpLightProjectionT;
	float4 rpLightProjectionQ;
	float4 rpLightFalloffS;
	float4 rpBumpMatrixS;
	float4 rpBumpMatrixT;
	float4 rpDiffuseMatrixS;
	float4 rpDiffuseMatrixT;
	float4 rpSpecularMatrixS;
	float4 rpSpecularMatrixT;
#if !SKINNED
	float4 rpVertexColorModulate;
	float4 rpVertexColorAdd;
#endif
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

cbuffer PixelParms : register( b1 ) {
	float4 rpDiffuseModifier;
	float4 rpSpecularModifier;
};

TEXTURE2D( Texture2D, bumpMap, 0 );
TEXTURE2D( Texture2D, lightFalloffMap, 1 );
TEXTURE2D( Texture2D, lightProjectionMap, 2 );
TEXTURE2D( Texture2D, diffuseMap, 3 );
TEXTURE2D( Texture2D, specularMap, 4 );
SAMPLER( bumpSampler, 0 );
SAMPLER( lightFalloffSampler, 1 );
SAMPLER( lightProjectionSampler, 2 );
SAMPLER( diffuseSampler, 3 );
SAMPLER( specularSampler, 4 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 normal : NORMAL;
	float4 tangent : TANGENT;
	float4 color : COLOR0;
#if SKINNED
	float4 color2 : COLOR1;
#endif
};

struct VSOutput {
	float4 position : SV_Position;
	float4 lightVector : TEXCOORD0;		// tangent space
	float4 bumpUV : TEXCOORD1;
	float4 lightFalloff : TEXCOORD2;
	float4 lightProjection : TEXCOORD3;
	float4 diffuseUV : TEXCOORD4;
	float4 specularUV : TEXCOORD5;
	float4 halfAngle : TEXCOORD6;		// tangent space
	float4 color : COLOR0;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output = (VSOutput)0;
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

	const float4 defaultTexCoord = float4( 0.0, 0.5, 0.0, 1.0 );
	float4 toLight = rpLocalLightOrigin - modelPosition;
	output.lightVector = float4( dot3( tangent, toLight ), dot3( binormal, toLight ), dot3( normal, toLight ), 1.0 );

	output.bumpUV = defaultTexCoord;
	output.bumpUV.x = dot4( input.texcoord.xy, rpBumpMatrixS );
	output.bumpUV.y = dot4( input.texcoord.xy, rpBumpMatrixT );

	output.lightFalloff = defaultTexCoord;
	output.lightFalloff.x = dot4( modelPosition, rpLightFalloffS );

	output.lightProjection.x = dot4( modelPosition, rpLightProjectionS );
	output.lightProjection.y = dot4( modelPosition, rpLightProjectionT );
	output.lightProjection.z = 0.0;
	output.lightProjection.w = dot4( modelPosition, rpLightProjectionQ );

	output.diffuseUV = defaultTexCoord;
	output.diffuseUV.x = dot4( input.texcoord.xy, rpDiffuseMatrixS );
	output.diffuseUV.y = dot4( input.texcoord.xy, rpDiffuseMatrixT );

	output.specularUV = defaultTexCoord;
	output.specularUV.x = dot4( input.texcoord.xy, rpSpecularMatrixS );
	output.specularUV.y = dot4( input.texcoord.xy, rpSpecularMatrixT );

	toLight = normalize( toLight );
	const float4 toView = normalize( rpLocalViewOrigin - modelPosition );
	const float4 halfAngleVector = toLight + toView;
	output.halfAngle = float4( dot3( tangent, halfAngleVector ), dot3( binormal, halfAngleVector ), dot3( normal, halfAngleVector ), 1.0 );

#if SKINNED
	output.color = float4( 1.0, 1.0, 1.0, 1.0 );
#else
	output.color = saturate( ( input.color * rpVertexColorModulate ) + rpVertexColorAdd );
#endif
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float4 bump = bumpMap.Sample( bumpSampler, input.bumpUV.xy );
	const float4 lightFalloff = SampleProjected( lightFalloffMap, lightFalloffSampler, input.lightFalloff );
	const float4 lightProjection = SampleProjected( lightProjectionMap, lightProjectionSampler, input.lightProjection );
	const float4 YCoCG = diffuseMap.Sample( diffuseSampler, input.diffuseUV.xy );
	const float4 specMap = specularMap.Sample( specularSampler, input.specularUV.xy );

	const float3 lightVector = normalize( input.lightVector.xyz );
	const float3 diffuse = ConvertYCoCgToRGB( YCoCG );

	float3 localNormal;
	localNormal.xy = bump.wy - 0.5;
	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );
	localNormal = normalize( localNormal );

	const float specularPower = 10.0;
	const float hDotN = dot3( normalize( input.halfAngle.xyz ), localNormal );
	const float3 specularContribution = pow( abs( hDotN ), specularPower );	// an even power: GL's pow of a negative base

	const float3 diffuseColor = diffuse * rpDiffuseModifier.xyz;
	const float3 specularColor = specMap.xyz * specularContribution * rpSpecularModifier.xyz;
	const float3 lightColor = dot3( lightVector, localNormal ) * lightProjection.xyz * lightFalloff.xyz;

	return float4( ( diffuseColor + specularColor ) * lightColor * input.color.xyz, 1.0 );
}
