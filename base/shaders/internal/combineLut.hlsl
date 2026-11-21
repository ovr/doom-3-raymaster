// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The Combine LUT of the HDR resolve: one thread per texel of a 32^3 LUT that maps
	the exposed scene colour (linear Rec.709) to the sRGB-encoded output. Per texel:

	  decode       t = id / 31, c = k * ( 2^( t * L ) - 1 ): log spacing, c = 1 exactly on texel 17 (d3d12_Hdr.h)
	  white bal.   a 3 x 3 Bradford adaptation from the white of r_hdrWhiteTemp / Tint to the white of 6500 K
	  grade        saturation, contrast, gamma, gain and offset for shadows, midtones and
	               highlights, the global values folded in on the CPU, weighted by luma
	  curve        0 clamp, 1 AgX (minimal AgX, base or punchy look), 2 a film curve in ACEScg
	  output       sRGB: AgX's curve is display-encoded already, the others go through the sRGB OETF
*/

#include "../include/srgb.hlsli"

cbuffer LutParms : register( b0 ) {
	float4 whiteBalance0;	// rows of the matrix; w unused
	float4 whiteBalance1;
	float4 whiteBalance2;
	float4 grade[15];		// [region * 5 + n]: saturation, contrast, gamma, gain, offset; shadows, midtones, highlights
	float4 options;			// x shadowsMax, y highlightsMin, z tonemapper, w AgX look
	float4 encoding;		// x k, y L, z white balance on, w grade on
};

cbuffer LutTarget : register( b1 ) {
	uint lutUav;
};

static const float3 LUMA_709 = float3( 0.2126, 0.7152, 0.0722 );

float3 ColorCorrect( float3 c, uint region ) {
	const float4 saturation = grade[region * 5 + 0];
	const float4 contrast = grade[region * 5 + 1];
	const float4 gamma = grade[region * 5 + 2];
	const float4 gain = grade[region * 5 + 3];
	const float4 offset = grade[region * 5 + 4];
	const float luma = dot( c, LUMA_709 );
	c = max( 0.0, lerp( luma.xxx, c, saturation.xyz * saturation.w ) );
	c = pow( c * ( 1.0 / 0.18 ), contrast.xyz * contrast.w ) * 0.18;
	c = pow( c, 1.0 / ( gamma.xyz * gamma.w ) );
	return c * ( gain.xyz * gain.w ) + ( offset.xyz + offset.w );
}

float3 ColorCorrectAll( float3 c ) {
	const float luma = dot( c, LUMA_709 );
	const float shadows = 1.0 - smoothstep( 0.0, options.x, luma );
	const float highlights = smoothstep( options.y, 1.0, luma );
	const float midtones = 1.0 - shadows - highlights;
	return ColorCorrect( c, 0 ) * shadows + ColorCorrect( c, 1 ) * midtones + ColorCorrect( c, 2 ) * highlights;
}

// Minimal AgX (Benjamin Wrensch's fit of Troy Sobotka's AgX): the inset matrix, a log2 encoding of -12.47 to
// +4.03 EV around 0.18, the sigmoid as a polynomial, the look as an ASC CDL, then the outset matrix.
static const float3x3 AGX_INSET = float3x3(
	0.842479062253094, 0.0423282422610123, 0.0423756549057051,
	0.0784335999999992, 0.878468636469772, 0.0784336,
	0.0792237451477643, 0.0791661274605434, 0.879142973793104 );
static const float3x3 AGX_OUTSET = float3x3(
	1.19687900512017, -0.0528968517574562, -0.0529716355144438,
	-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
	-0.0990297440797205, -0.0989611768448433, 1.15107367264116 );

float3 AgxSigmoid( float3 x ) {
	const float3 x2 = x * x;
	const float3 x4 = x2 * x2;
	return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 Agx( float3 c, bool punchy ) {
	const float minEv = -12.47393;
	const float maxEv = 4.026069;
	c = mul( max( c, 1e-10 ), AGX_INSET );		// the rows are the columns of the GLSL original
	c = ( clamp( log2( c ), minEv, maxEv ) - minEv ) / ( maxEv - minEv );
	c = AgxSigmoid( c );
	if ( punchy ) {
		const float luma = dot( c, LUMA_709 );
		c = pow( max( c, 0.0 ), 1.35 );
		c = luma + 1.4 * ( c - luma );
	}
	return mul( c, AGX_OUTSET );
}

// The filmic curve of UE4/5 (a fit of the ACES 1.0 RRT + ODT): in ACEScg (AP1), a log10 toe, a straight
// segment and a shoulder, desaturated before and after. Without the RRT's glow and red modifier.
static const float3x3 SRGB_TO_AP1 = float3x3(
	0.613097, 0.339523, 0.047379,
	0.070194, 0.916354, 0.013452,
	0.020616, 0.109570, 0.869815 );
static const float3x3 AP1_TO_SRGB = float3x3(
	1.704859, -0.621715, -0.083299,
	-0.130078, 1.140734, -0.010560,
	-0.023964, -0.128975, 1.153013 );
static const float3 AP1_LUMA = float3( 0.2722287168, 0.6740817658, 0.0536895174 );

float3 Filmic( float3 c ) {
	const float slope = 0.88;
	const float toe = 0.55;
	const float shoulder = 0.26;
	const float blackClip = 0.0;
	const float whiteClip = 0.04;

	float3 color = max( 0.0, mul( SRGB_TO_AP1, c ) );
	color = lerp( dot( color, AP1_LUMA ).xxx, color, 0.96 );

	// 0.18 in gives 0.18 out on the toe
	const float toeScale = 1.0 + blackClip - toe;
	const float shoulderScale = 1.0 + whiteClip - shoulder;
	const float bt = ( 0.18 + blackClip ) / toeScale - 1.0;
	const float toeMatch = log10( 0.18 ) - 0.5 * log( ( 1.0 + bt ) / ( 1.0 - bt ) ) * ( toeScale / slope );
	const float straightMatch = ( 1.0 - toe ) / slope - toeMatch;
	const float shoulderMatch = shoulder / slope - straightMatch;

	const float3 logColor = log10( max( color, 1e-10 ) );
	const float3 straightColor = slope * ( logColor + straightMatch );
	float3 toeColor = -blackClip + ( 2.0 * toeScale ) / ( 1.0 + exp( ( -2.0 * slope / toeScale ) * ( logColor - toeMatch ) ) );
	float3 shoulderColor = ( 1.0 + whiteClip ) - ( 2.0 * shoulderScale ) / ( 1.0 + exp( ( 2.0 * slope / shoulderScale ) * ( logColor - shoulderMatch ) ) );
	toeColor = select( logColor < toeMatch, toeColor, straightColor );
	shoulderColor = select( logColor > shoulderMatch, shoulderColor, straightColor );

	float3 t = saturate( ( logColor - toeMatch ) / ( shoulderMatch - toeMatch ) );
	t = ( shoulderMatch < toeMatch ) ? 1.0 - t : t;
	t = ( 3.0 - 2.0 * t ) * t * t;
	float3 toned = lerp( toeColor, shoulderColor, t );
	toned = lerp( dot( toned, AP1_LUMA ).xxx, toned, 0.93 );
	return max( 0.0, mul( AP1_TO_SRGB, max( 0.0, toned ) ) );
}

[numthreads( 4, 4, 4 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id >= 32 ) ) {
		return;
	}
	RWTexture3D<float4> lut = ResourceDescriptorHeap[lutUav];
	float3 c = encoding.x * ( exp2( ( float3( id ) / 31.0 ) * encoding.y ) - 1.0 );
	if ( encoding.z != 0.0 ) {
		c = max( 0.0, float3( dot( whiteBalance0.xyz, c ), dot( whiteBalance1.xyz, c ), dot( whiteBalance2.xyz, c ) ) );
	}
	if ( encoding.w != 0.0 ) {
		c = max( 0.0, ColorCorrectAll( c ) );
	}
	float3 output;
	const uint tonemapper = uint( options.z );
	if ( tonemapper == 1 ) {
		output = Agx( c, options.w != 0.0 );
	} else if ( tonemapper == 2 ) {
		output = LinearToSRGB( saturate( Filmic( c ) ) );
	} else {
		output = LinearToSRGB( saturate( c ) );
	}
	lut[id] = float4( saturate( output ), 1.0 );
}
