// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The tonemap of the HDR resolve: the scene colour (radiance times preExposure) times the eye adaptation's
	exposure / preExposure, through the Combine LUT by its log coordinate, dithered by half a step of the 8-bit
	output from a hash of the pixel, so two runs give the same bytes.

	  visualize 1   over the image, in the viewport's top left: the histogram (the bins averaged bright), the
	                measured (cyan), target (yellow) and adapted (green) EV100 as lines and numbers, then the
	                exposure and the preExposure in EV (white, grey)
	  visualize 2   instead of the image: the exposed luminance in stops around mid grey, a colour per stop from
	                dark blue (-5) over green (0) to red (+4) and white
*/

#include "../include/exposure.hlsli"
#include "../include/srgb.hlsli"

cbuffer TonemapParms : register( b1 ) {
	uint sceneSrv;			// RGBA16F, one sample: the scene or its MSAA resolve
	uint lutSrv;			// Texture3D 32^3
	uint lutSampler;		// linear, clamp
	uint dither;
	float invPreExposure;	// 1 / the preExposure the scene was drawn with
	float lutK;				// the encoding of d3d12_Hdr.h: t = log2( c / k + 1 ) / L
	float lutInvRange;		// 1 / L
	uint eyeSrv;			// EyeAdaptation, include/exposure.hlsli
	uint visualize;
	float histogramMinEV;
	float binWidth;			// EV per bin
	float lowPercent;
	float highPercent;
	int viewX;				// the viewport, rows from the bottom
	int viewY;
	int viewWidth;
	int viewHeight;
};

// Jimenez's interleaved gradient noise: 0..1, no frame index.
float Noise( float2 pixel ) {
	return frac( 52.9829189 * frac( dot( pixel, float2( 0.06711056, 0.00583715 ) ) ) );
}

float3 FalseColor( float luminance ) {
	static const float3 STOPS[11] = { float3( 0.05, 0.0, 0.15 ), float3( 0.25, 0.0, 0.5 ), float3( 0.0, 0.1, 0.85 ), float3( 0.0, 0.45, 0.95 ),
		float3( 0.0, 0.75, 0.75 ), float3( 0.25, 0.8, 0.25 ), float3( 0.75, 0.85, 0.0 ), float3( 1.0, 0.75, 0.0 ), float3( 1.0, 0.45, 0.0 ),
		float3( 0.95, 0.1, 0.1 ), float3( 1.0, 0.9, 0.95 ) };
	const float stops = clamp( log2( max( luminance, 1e-10 ) / 0.18 ), -5.0, 5.0 );
	return STOPS[(uint)round( stops + 5.0 )];
}

// 3 x 5 glyphs, rows from the top, the left column in the high bit: 0-9, minus, dot; 12 is blank.
static const uint GLYPHS[13] = { 0x7B6F, 0x2C97, 0x73E7, 0x72CF, 0x5BC9, 0x79CF, 0x79EF, 0x7252, 0x7BEF, 0x7BCF, 0x01C0, 0x0002, 0 };
static const uint GLYPH_MINUS = 10;
static const uint GLYPH_DOT = 11;
static const uint GLYPH_BLANK = 12;

// Character n of value printed as -ddd.dd in 7 characters, right-aligned.
uint NumberGlyph( float value, uint n ) {
	const uint hundredths = (uint)( min( abs( value ), 999.99 ) * 100.0 + 0.5 );
	const uint whole = hundredths / 100;
	const uint first = ( whole >= 100 ) ? 1 : ( ( whole >= 10 ) ? 2 : 3 );
	if ( n < first ) {
		return ( n + 1 == first && value < 0.0 && hundredths != 0 ) ? GLYPH_MINUS : GLYPH_BLANK;
	}
	if ( n <= 3 ) {
		const uint power = ( n == 1 ) ? 100 : ( ( n == 2 ) ? 10 : 1 );
		return ( whole / power ) % 10;
	}
	return ( n == 4 ) ? GLYPH_DOT : ( ( n == 5 ) ? ( hundredths / 10 ) % 10 : hundredths % 10 );
}

bool GlyphPixel( uint glyph, int2 p ) {
	return all( p >= 0 ) && p.x < 3 && p.y < 5 && ( ( GLYPHS[glyph] >> ( ( 4 - p.y ) * 3 + ( 2 - p.x ) ) ) & 1 ) != 0;
}

// The overlay of visualize 1 at u, in units from the panel's top left.
float3 Overlay( float3 color, int2 u ) {
	const int2 PANEL = int2( 264, 143 );
	if ( any( u < 0 ) || any( u >= PANEL ) ) {
		return color;
	}
	ByteAddressBuffer eye = ResourceDescriptorHeap[eyeSrv];
	const float4 state = asfloat( eye.Load4( 0 ) );		// exposure, measured, adapted, target
	float3 result = color * 0.25;
	if ( u.x >= 4 && u.x < 260 && u.y >= 4 && u.y < 100 ) {
		const uint bin = (uint)( u.x - 4 ) / 4;
		uint total = 0;
		uint most = 1;
		uint below = 0;
		for ( uint b = 0; b < EXPOSURE_BINS; b++ ) {
			const uint count = eye.Load( EYE_HISTOGRAM + b * 4 );
			total += count;
			most = max( most, count );
			below += ( b < bin ) ? count : 0;
		}
		const uint count = eye.Load( EYE_HISTOGRAM + bin * 4 );
		const bool averaged = float( below + count ) > float( total ) * lowPercent && float( below ) < float( total ) * highPercent;
		if ( ( u.x - 4 ) % 4 != 3 && float( 100 - u.y ) <= 96.0 * float( count ) / float( most ) ) {
			result = averaged ? float3( 0.85, 0.85, 0.85 ) : float3( 0.35, 0.35, 0.35 );
		}
		const float3 lines[3] = { float3( 0.3, 0.9, 1.0 ), float3( 0.3, 1.0, 0.3 ), float3( 1.0, 0.85, 0.2 ) };
		for ( uint m = 0; m < 3; m++ ) {
			const float ev = ( m == 0 ) ? state.y : ( ( m == 1 ) ? state.z : state.w );
			if ( u.x == 4 + (int)floor( ( ev - histogramMinEV ) / binWidth * 4.0 ) ) {
				result = lines[m];
			}
		}
		return result;
	}
	const int row = ( u.y - 104 ) / 7;
	if ( u.y >= 104 && row < 5 ) {
		const float3 colors[5] = { float3( 0.3, 0.9, 1.0 ), float3( 1.0, 0.85, 0.2 ), float3( 0.3, 1.0, 0.3 ), float3( 1.0, 1.0, 1.0 ), float3( 0.6, 0.6, 0.6 ) };
		const float values[5] = { state.y, state.w, state.z, log2( max( state.x, 1e-30 ) ), -log2( invPreExposure ) };
		const int y = u.y - 104 - row * 7;
		if ( u.x >= 4 && u.x < 9 && y < 5 ) {
			return colors[row];
		}
		if ( u.x >= 12 && u.x < 40 ) {
			const uint n = (uint)( u.x - 12 ) / 4;
			if ( GlyphPixel( NumberGlyph( values[row], n ), int2( ( u.x - 12 ) % 4, y ) ) ) {
				return colors[row];
			}
		}
	}
	return result;
}

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	Texture2D<float4> scene = ResourceDescriptorHeap[sceneSrv];
	Texture3D<float4> lut = ResourceDescriptorHeap[lutSrv];
	SamplerState lutFilter = SamplerDescriptorHeap[lutSampler];
	ByteAddressBuffer eye = ResourceDescriptorHeap[eyeSrv];
	const float exposureScale = asfloat( eye.Load( 0 ) ) * invPreExposure;
	const float4 sceneColor = scene.Load( int3( position.xy, 0 ) );
	const float3 c = clamp( sceneColor.rgb * exposureScale, 0.0, HALF_MAX );	// no NaN or infinity reaches the LUT
	float3 color;
	if ( visualize == 2 ) {
		color = FalseColor( dot( c, EXPOSURE_LUMA ) );
	} else {
		const float3 t = saturate( log2( c / lutK + 1.0 ) * lutInvRange );
		color = lut.SampleLevel( lutFilter, t * ( 31.0 / 32.0 ) + 0.5 / 32.0, 0 ).rgb;
		if ( dither != 0 ) {
			color += ( Noise( position.xy ) - 0.5 ) / 255.0;
		}
	}
	color = saturate( color );
	if ( visualize == 1 ) {
		const int scale = max( 1, viewHeight / 540 );
		const int2 pixel = int2( position.xy );
		const int2 fromTop = int2( pixel.x - viewX, viewY + viewHeight - 1 - pixel.y );		// the rows run from the bottom
		color = Overlay( color, fromTop / scale - 16 );
	}
	return float4( color, saturate( sceneColor.a ) );
}
