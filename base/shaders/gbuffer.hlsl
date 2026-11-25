// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The thin G-buffer (include/gbuffer.hlsli), drawn depth-equal after the depth pass, so the clip position is
// the depth program's to the bit. SKINNED=1 is gbuffer_skinned: rpJointMotion.x is the row of the previous
// frame's joints in the same block, 0 when the model has none.

#include "include/common.hlsli"
#include "include/gbuffer.hlsli"
#if SKINNED
#define JOINT_VECTORS 816
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpModelMatrixX;
	float4 rpModelMatrixY;
	float4 rpModelMatrixZ;
	float4 rpPrevMVPmatrixX;
	float4 rpPrevMVPmatrixY;
	float4 rpPrevMVPmatrixW;
	float4 rpBumpMatrixS;
	float4 rpBumpMatrixT;
	float4 rpSpecularMatrixS;
	float4 rpSpecularMatrixT;
	float4 rpGlobalEyePos;
#if SKINNED
	float4 rpJointMotion;
#endif
};

// rpGBufferParms: x the gloss scale, y flags (1 may reflect, 2 two-sided), zw half the viewport in pixels
cbuffer PixelParms : register( b1 ) {
	float4 rpSpecularModifier;
	float4 rpGBufferParms;
};

TEXTURE2D( Texture2D, bumpMap, 0 );
TEXTURE2D( Texture2D, specularMap, 1 );
SAMPLER( bumpSampler, 0 );
SAMPLER( specularSampler, 1 );

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

struct VSOutput {
	float4 position : SV_Position;
	float4 uv : TEXCOORD0;				// xy bump, zw specular
	float3 tangent : TEXCOORD1;			// world space
	float3 binormal : TEXCOORD2;
	float3 normal : TEXCOORD3;
	float3 toEye : TEXCOORD4;
	float3 clip : TEXCOORD5;			// GL clip xyw of this frame
	float3 prevClip : TEXCOORD6;		// and of the previous one
};

float3 ModelToWorld( float3 v ) {
	return float3( dot3( rpModelMatrixX, v ), dot3( rpModelMatrixY, v ), dot3( rpModelMatrixZ, v ) );
}

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
	precise const float4 prevPosition = SkinPosition( SkinningMatrixAt( input.color, input.color2, int( rpJointMotion.x ) ), input.position );
#else
	const float3 normal = vNormal.xyz;
	const float3 tangent = vTangent.xyz;
	const float3 binormal = vBinormal;
	const float4 modelPosition = input.position;
	const float4 prevPosition = input.position;
#endif

	float4 glPosition;
	glPosition.x = dot4( modelPosition, rpMVPmatrixX );
	glPosition.y = dot4( modelPosition, rpMVPmatrixY );
	glPosition.z = dot4( modelPosition, rpMVPmatrixZ );
	glPosition.w = dot4( modelPosition, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;

	// both the same way, so a still surface has no motion to the bit
	precise float3 clip = glPosition.xyw;
	precise float3 prevClip = float3( dot4( prevPosition, rpPrevMVPmatrixX ), dot4( prevPosition, rpPrevMVPmatrixY ),
		dot4( prevPosition, rpPrevMVPmatrixW ) );
	output.clip = clip;
	output.prevClip = prevClip;

	output.uv.x = dot4( input.texcoord.xy, rpBumpMatrixS );
	output.uv.y = dot4( input.texcoord.xy, rpBumpMatrixT );
	output.uv.z = dot4( input.texcoord.xy, rpSpecularMatrixS );
	output.uv.w = dot4( input.texcoord.xy, rpSpecularMatrixT );

	output.tangent = ModelToWorld( tangent );
	output.binormal = ModelToWorld( binormal );
	output.normal = ModelToWorld( normal );
	const float3 worldPosition = float3( dot4( modelPosition, rpModelMatrixX ), dot4( modelPosition, rpModelMatrixY ),
		dot4( modelPosition, rpModelMatrixZ ) );
	output.toEye = rpGlobalEyePos.xyz - worldPosition;
	return output;
}

struct PSOutput {
	float2 normal : SV_Target0;
	float4 material : SV_Target1;
	float2 motion : SV_Target2;
};

PSOutput PSMain( VSOutput input ) {
	const float4 bump = bumpMap.Sample( bumpSampler, input.uv.xy );
	const float4 specMap = DecodeColor( specularMap.Sample( specularSampler, input.uv.zw ) );
	const uint flags = uint( rpGBufferParms.y );

	// the decode of interaction.hlsl
	float3 localNormal;
	localNormal.xy = bump.wy - 0.5;
	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );
	localNormal = normalize( localNormal );

	float3 normal = normalize( input.normal );
	float3 worldNormal = localNormal.x * input.tangent + localNormal.y * input.binormal + localNormal.z * input.normal;
	if ( ( flags & 2 ) != 0 && dot( normal, input.toEye ) < 0.0 ) {
		worldNormal = -worldNormal;
	}

	PSOutput output;
	output.normal = EncodeOctahedral( normalize( worldNormal ) );

	const float3 f0 = ( ( flags & 1 ) != 0 ) ? saturate( specMap.rgb * rpSpecularModifier.rgb ) : float3( 0.0, 0.0, 0.0 );
	const float gloss = saturate( sqrt( dot( f0, float3( 0.2126, 0.7152, 0.0722 ) ) ) * rpGBufferParms.x );
	output.material = float4( f0, 1.0 - gloss );

	output.motion = float2( 0.0, 0.0 );
	if ( input.prevClip.z > 0.0 ) {
		output.motion = ( input.clip.xy / input.clip.z - input.prevClip.xy / input.prevClip.z ) * rpGBufferParms.zw;
	}
	return output;
}
