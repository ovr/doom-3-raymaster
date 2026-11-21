// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "bindless.hlsli"
#include "srgb.hlsli"

/*
	The colour rules of the HDR scene (R13 of RenderBackend.h). The scene holds radiance times preExposure in
	RGBA16F. The lights add in linear space; the passes after them (unlit stages, translucent surfaces, fog
	and blend lights) blend in gamma as id's content was made for, over the scene's sRGB encoding (the gamma
	composite, internal/hdrEncode.hlsl). colorFlags of b3 say how a draw's colours are read and written:

	  bits 0-7   slot n holds a float copy of the HDR scene (radiance times preExposure), not 8-bit colour
	  bit 8      the linear phase: the draw goes into the linear HDR scene
	  bit 9      the output is radiance that the blend adds; without it, a factor the blend multiplies by
	  bit 10     the gamma phase: the draw goes into the scene's sRGB encoding

	  DecodeColor( c )         an 8-bit colour (texture, parm, vertex colour) to linear in the linear phase
	  SceneColor( c, slot )    a sample of slot: 8-bit decoded in the linear phase; a float copy divided by
	                           preExposure, encoded to sRGB outside the linear phase
	  SceneOutput( c )         every PS result in either phase: rgb >= 0 (and no NaN), exposed by preExposure
	                           for radiance, alpha saturated as a UNORM target would

	Geometry (N.L, the specular power) and data (normals, fog density, masks) are never decoded. A vertex
	colour is decoded before the inverse: c and 1 - c still sum to 1. Outside both phases, with no float
	slot, every function returns its input, so the LDR frames stay as they were.
*/

static const uint COLOR_FLOAT_SLOTS = 0xFF;
static const uint COLOR_HDR_PHASE = 1u << 8;
static const uint COLOR_RADIANCE = 1u << 9;
static const uint COLOR_GAMMA_PHASE = 1u << 10;

// The linear phase: the colours decode.
bool HdrPhase() {
	return ( colorFlags & COLOR_HDR_PHASE ) != 0;
}

bool GammaPhase() {
	return ( colorFlags & COLOR_GAMMA_PHASE ) != 0;
}

float3 DecodeColor( float3 c ) {
	return HdrPhase() ? SRGBToLinear( c ) : c;
}

float4 DecodeColor( float4 c ) {
	return float4( DecodeColor( c.rgb ), c.a );
}

float4 SceneColor( float4 c, uint slot ) {
	if ( ( ( colorFlags >> slot ) & 1 ) != 0 ) {
		c.rgb /= preExposure;
		if ( !HdrPhase() ) {
			c.rgb = LinearToSRGB( GammaPhase() ? max( c.rgb, 0.0 ) : saturate( c.rgb ) );	// the gamma phase keeps the HDR
		}
	} else if ( HdrPhase() ) {
		c.rgb = SRGBToLinear( c.rgb );
	}
	return c;
}

// A sample in sRGB whatever the slot holds: for programs whose math is defined in gamma (the GUI, video).
float4 SceneColorGamma( float4 c, uint slot ) {
	if ( ( ( colorFlags >> slot ) & 1 ) != 0 ) {
		c.rgb = LinearToSRGB( saturate( c.rgb / preExposure ) );
	}
	return c;
}

float4 SceneOutput( float4 c ) {
	if ( HdrPhase() || GammaPhase() ) {
		c.rgb = max( c.rgb, 0.0 );
		if ( ( colorFlags & COLOR_RADIANCE ) != 0 && preExposure != 1.0 ) {
			c.rgb = HdrPhase() ? c.rgb * preExposure : LinearToSRGB( SRGBToLinear( c.rgb ) * preExposure );
		}
		c.rgb = min( c.rgb, HALF_MAX );
		c.a = saturate( c.a );
	}
	return c;
}
