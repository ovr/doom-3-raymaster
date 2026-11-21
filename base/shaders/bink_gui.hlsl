// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A Bink video frame in a GUI: Y, Cr and Cb to RGB, then gui's vertex colour and signed add, premultiplied (renderprogs/bink_gui).

#include "include/common.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

TEXTURE2D( Texture2D, yMap, 0 );
TEXTURE2D( Texture2D, crMap, 1 );
TEXTURE2D( Texture2D, cbMap, 2 );
SAMPLER( ySampler, 0 );
SAMPLER( crSampler, 1 );
SAMPLER( cbSampler, 2 );

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
	output.texcoord = input.texcoord.xy;
	output.add = ( input.color2 * 2 ) - 1;
	output.color = saturate( input.color );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float3 crc = float3( 1.595794678, -0.813476563, 0 );
	const float3 crb = float3( 0, -0.391448975, 2.017822266 );
	const float3 adj = float3( -0.87065506, 0.529705048, -1.081668854 );
	const float3 YScalar = float3( 1.164123535, 1.164123535, 1.164123535 );

	const float Y = yMap.Sample( ySampler, input.texcoord.xy ).x;
	const float Cr = crMap.Sample( crSampler, input.texcoord.xy ).x;
	const float Cb = cbMap.Sample( cbSampler, input.texcoord.xy ).x;

	float3 p = ( YScalar * Y );
	p += ( crc * Cr ) + ( crb * Cb ) + adj;

	float4 binkImage;
	binkImage.xyz = p;
	binkImage.w = 1.0;

	float4 color = ( binkImage * input.color ) + input.add;
	if ( HdrPhase() ) {
		color = float4( SRGBToLinear( saturate( color.xyz ) ), saturate( color.w ) );	// the GUI formula is in gamma
	}
	return SceneOutput( float4( color.xyz * color.w, color.w ) );
}
