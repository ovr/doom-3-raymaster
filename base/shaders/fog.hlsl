// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// Fog: the fog by distance times its fade at the entering plane, times rpColor (renderprogs/fog).
// SKINNED=1 is fog_skinned: the joints in COLOR0 and COLOR1 move the position.

#include "include/common.hlsli"
#if SKINNED
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpTexGen0S;
	float4 rpTexGen0T;
	float4 rpTexGen1S;
	float4 rpTexGen1T;
};

cbuffer PixelParms : register( b1 ) {
	float4 rpColor;
};

TEXTURE2D( Texture2D, fogMap, 0 );
TEXTURE2D( Texture2D, fogEnterMap, 1 );
SAMPLER( fogSampler, 0 );
SAMPLER( fogEnterSampler, 1 );

struct VSInput {
	float4 position : POSITION;
#if SKINNED
	float4 color : COLOR0;
	float4 color2 : COLOR1;
#endif
};

struct VSOutput {
	float4 position : SV_Position;
	float2 fogUV : TEXCOORD0;
	float2 fogEnterUV : TEXCOORD1;
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

	output.fogUV.x = dot4( modelPosition, rpTexGen0S );
	output.fogUV.y = dot4( modelPosition, rpTexGen0T );

	output.fogEnterUV.x = dot4( modelPosition, rpTexGen1S );
	output.fogEnterUV.y = dot4( modelPosition, rpTexGen1T );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	return fogMap.Sample( fogSampler, input.fogUV ) * fogEnterMap.Sample( fogEnterSampler, input.fogEnterUV ) * rpColor;
}
