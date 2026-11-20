// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// Heat haze where a mask lets it through: the screen shifted by a scrolling bump map (renderprogs/heatHazeWithMask).

#include "include/common.hlsli"
#include "include/skinning.hlsli"
#include "include/heatHaze.hlsli"

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

Texture2D currentRenderMap : register( t0 );
Texture2D bumpMap : register( t1 );
Texture2D maskMap : register( t2 );
SamplerState currentRenderSampler : register( s0 );
SamplerState bumpSampler : register( s1 );
SamplerState maskSampler : register( s2 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
	float4 color2 : COLOR1;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output = (VSOutput)0;
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

	// the mask takes the texture coordinates unmodified, the bump map adds a scroll
	output.texcoord0 = float4( input.texcoord.xy, 0, 0 );

	const float4 textureScroll = rpUser0;
	output.texcoord1 = float4( input.texcoord.xy, 0, 0 ) + textureScroll;

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
	output.texcoord2 = x * deformMagnitude;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	float4 mask = maskMap.Sample( maskSampler, input.texcoord0.xy );

	// kill the pixel if the distortion wound up being very small
	mask.xy -= 0.01;
	clip( mask );

	// the filtered normal map in the -1 to 1 range
	const float4 bump = ( bumpMap.Sample( bumpSampler, input.texcoord1.xy ) * 2.0 ) - 1.0;
	float2 localNormal = bump.wy;
	localNormal *= mask.xy;

	// the screen texcoord in the 0.0 to 1.0 range
	float2 screenTexCoord = FragCoord( input.position ).xy * rpWindowCoord.xy;
	screenTexCoord += ( localNormal * input.texcoord2.xy );
	screenTexCoord = saturate( screenTexCoord );

	return currentRenderMap.Sample( currentRenderSampler, screenTexCoord );
}
