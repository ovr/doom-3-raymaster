// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A texture times the vertex colour, alpha-tested, through the texture matrix or the texgen (renderprogs/texture_color).
// SKINNED=1 is texture_color_skinned: the joints in COLOR0, which is still the vertex colour, and the weights in COLOR1.
// SOFT_PARTICLE=1 is texture_color_soft: it fades where it meets the scene's depth (RB_BindSoftParticle).

#include "include/common.hlsli"
#if SKINNED
#include "include/skinning.hlsli"
#endif

cbuffer VertexParms : register( b0 ) {
	float4 rpVertexColorModulate;
	float4 rpVertexColorAdd;
	float4 rpColor;
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpTextureMatrixS;
	float4 rpTextureMatrixT;
	float4 rpTexGen0S;
	float4 rpTexGen0T;
	float4 rpTexGen0Enabled;
};

cbuffer PixelParms : register( b1 ) {
#if SOFT_PARTICLE
	float4 rpProjectionMatrixZ;
#endif
	float4 rpAlphaTest;
#if SOFT_PARTICLE
	float4 rpSoftParticle;		// 1 / fade depth, GLS_SoftParticleMode, the viewport's origin (texel 0 of the copy)
#endif
};

TEXTURE2D( Texture2D, textureMap, 0 );
SAMPLER( textureSampler, 0 );

#if SOFT_PARTICLE
TEXTURE2D( Texture2D, viewDepthMap, 1 );	// the view's depth copy: GL window z

// idRenderMatrix::ApplyDepthHack: the weapon's clip z is a quarter of the true one
static const float WEAPON_DEPTH_SCALE = 0.25;

// as motionBlur.hlsl; the far clear (z 1) lies past the plane of GL's infinite projection (-0.999): no depth
float ViewDepth( float windowZ ) {
	const float denominator = -rpProjectionMatrixZ.z - ( windowZ * 2.0 - 1.0 );
	return ( denominator > 0.0 ) ? -rpProjectionMatrixZ.w / denominator : 1e30;
}
#endif

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
#if SKINNED
	float4 color2 : COLOR1;
#endif
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
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

	[branch] if ( rpTexGen0Enabled.x > 0.0 ) {
		output.texcoord.x = dot4( modelPosition, rpTexGen0S );
		output.texcoord.y = dot4( modelPosition, rpTexGen0T );
	} else {
		output.texcoord.x = dot4( input.texcoord.xy, rpTextureMatrixS );
		output.texcoord.y = dot4( input.texcoord.xy, rpTextureMatrixT );
	}

	const float4 vertexColor = ( DecodeColor( input.color ) * rpVertexColorModulate ) + rpVertexColorAdd;
	output.color = saturate( vertexColor * rpColor );
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	float4 color = SceneColor( textureMap.Sample( textureSampler, input.texcoord ), 0 ) * input.color;
	clip( color.a - rpAlphaTest.x );
#if SOFT_PARTICLE
	// the fragment's own z: a depth hack moves it, and the depth test sees the moved one
	const float sceneZ = viewDepthMap.Load( int3( int2( input.position.xy ) - int2( rpSoftParticle.zw ), 0 ) ).x;
	// the weapon's squeezed z reads as a surface a few units away: a scene z that close counts as far, so smoke in
	// front of the weapon still covers it (a true surface that close is within 2.7 times the near plane)
	const bool weapon = sceneZ * 2.0 - 1.0 <= WEAPON_DEPTH_SCALE * -rpProjectionMatrixZ.z;
	const float fade = weapon ? 1.0 : saturate( ( ViewDepth( sceneZ ) - ViewDepth( input.position.z ) ) * rpSoftParticle.x );
	const uint mode = uint( rpSoftParticle.y );
	if ( mode == 0 ) {
		color.rgb *= fade;
	} else if ( mode == 1 ) {
		color.a *= fade;
	} else if ( mode == 2 ) {
		color *= fade;
	} else {
		color.rgb = lerp( 1.0, color.rgb, fade );
	}
#endif
	return SceneOutput( color );
}
