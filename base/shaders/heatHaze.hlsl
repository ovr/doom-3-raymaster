// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The screen behind a surface, shifted by its scrolling bump map and scaled down with distance (renderprogs/heathaze).

#include "include/common.hlsli"
#include "include/skinning.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpProjectionMatrixY;
	float4 rpProjectionMatrixW;
	float4 rpModelViewMatrixZ;
	float4 rpEnableSkinning;
	float4 rpUser0;		// the scroll
	float4 rpUser1;		// the deform magnitude
};

cbuffer PixelParms : register( b1 ) {
	float4 rpWindowCoord;
};

TEXTURE2D( Texture2D, currentRenderMap, 0 );
TEXTURE2D( Texture2D, bumpMap, 1 );
SAMPLER( currentRenderSampler, 0 );
SAMPLER( bumpSampler, 1 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
	float4 color2 : COLOR1;
};

struct VSOutput {
	float4 position : SV_Position;
	float4 texcoord0 : TEXCOORD0;
	float4 texcoord1 : TEXCOORD1;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
	float4 modelPosition = input.position;
	if ( rpEnableSkinning.x > 0.0 ) {
		const SkinMatrix skin = SkinningMatrix( input.color, input.color2 );
		modelPosition = SkinPosition( skin, input.position );
	}

	float4 glPosition;
	glPosition.x = dot4( modelPosition, rpMVPmatrixX );
	glPosition.y = dot4( modelPosition, rpMVPmatrixY );
	glPosition.z = dot4( modelPosition, rpMVPmatrixZ );
	glPosition.w = dot4( modelPosition, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;

	const float4 textureScroll = rpUser0;
	output.texcoord0 = float4( input.texcoord.xy, 0, 0 ) + textureScroll;

	// the deform magnitude scaled by the projection distance
	float4 vec = float4( 0, 1, 0, 1 );
	vec.z = dot4( modelPosition, rpModelViewMatrixZ );

	// id's magic scalar: the projection moved from the X axis to the Y axis, this approximates the original game
	const float magicProjectionAdjust = 0.43;
	float x = dot4( vec, rpProjectionMatrixY ) * magicProjectionAdjust;
	float w = dot4( vec, rpProjectionMatrixW );

	w = max( w, 1.0 );	// the reciprocal stays away from zero for polygons that cross the view plane
	x /= w;

	x = min( x, 0.02 );	// so the deformations don't get too wacky near the view

	const float4 deformMagnitude = rpUser1;
	output.texcoord1 = x * deformMagnitude;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	// the filtered normal map in the -1 to 1 range
	const float4 bump = ( bumpMap.Sample( bumpSampler, input.texcoord0.xy ) * 2.0 ) - 1.0;
	const float2 localNormal = bump.wy;

	// the screen texcoord in the 0.0 to 1.0 range
	float2 screenTexCoord = FragCoord( input.position ).xy * rpWindowCoord.xy;
	screenTexCoord += ( localNormal * input.texcoord1.xy );
	screenTexCoord = saturate( screenTexCoord );

	return SceneOutput( SceneColor( currentRenderMap.Sample( currentRenderSampler, screenTexCoord.xy ), 0 ) );
}
