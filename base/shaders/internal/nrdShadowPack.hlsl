// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The traced shadows (R16) into NRD's IN_PENUMBRA and IN_TRANSLUCENCY of SIGMA_SHADOW_TRANSLUCENCY, rows from the
	top, as NRD's recipe for many lights has it: the translucency is what the shadows leave of the lights
	(internal/rtShadowRatio.hlsl, rows from the bottom), the penumbra the radius of the casters' penumbrae weighted by
	the luminance each takes (the light target's alpha over its colour's luminance, interaction_rt.hlsl), times
	d3d12_Nrd.cpp's SIGMA_PENUMBRA.
	NRD_FP16_MAX where no caster stands, 0 (a surface that faces away) where no traced light reaches.
	MULTISAMPLED averages the light target's samples.
*/

#include "../include/nrd/NRD.hlsli"

cbuffer PackParms : register( b0 ) {
	int2 first;				// the viewport's first pixel in the scene
	uint2 size;				// pixels
	uint ratioSrv;			// rgb what is left, a the unshadowed luminance
	uint lightsSrv;			// RGBA16F, the light target's samples
	uint samples;
	float penumbraScale;	// the light target's alpha over its luminance to the radius SIGMA blurs over
	uint penumbraUav;
	uint translucencyUav;
	uint unused0;
	uint unused1;
};

// World units: a hard shadow, which 0 would make a surface that faces away; the most SIGMA_FrontEnd_PackPenumbra gives.
static const float PENUMBRA_LEAST = 1.0 / 256.0;
static const float PENUMBRA_MOST = 32768.0;

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	Texture2D<float4> ratios = ResourceDescriptorHeap[ratioSrv];
	RWTexture2D<float> penumbrae = ResourceDescriptorHeap[penumbraUav];
	RWTexture2D<float4> translucencies = ResourceDescriptorHeap[translucencyUav];
	const int2 texel = int2( id.x, size.y - 1 - id.y );
	const int2 pixel = first + texel;
#if MULTISAMPLED
	Texture2DMS<float4> lightTarget = ResourceDescriptorHeap[lightsSrv];
	float4 lights = float4( 0.0, 0.0, 0.0, 0.0 );
	for ( uint i = 0; i < samples; i++ ) {
		lights += max( lightTarget.Load( pixel, i ), 0.0 );
	}
	lights /= float( samples );
#else
	Texture2D<float4> lightTarget = ResourceDescriptorHeap[lightsSrv];
	const float4 lights = max( lightTarget.Load( int3( pixel, 0 ) ), 0.0 );
#endif
	const float4 ratio = ratios.Load( int3( texel, 0 ) );
	const float taken = dot( lights.rgb, float3( 0.2126, 0.7152, 0.0722 ) );
	float penumbra = NRD_FP16_MAX;
	if ( !( ratio.a > 0.0 ) ) {
		penumbra = 0.0;
	} else if ( taken > 0.0 ) {
		// an alpha past the half float's range: the widest penumbra
		penumbra = isinf( lights.a ) ? PENUMBRA_MOST : clamp( lights.a * penumbraScale / taken, PENUMBRA_LEAST, PENUMBRA_MOST );
	}
	penumbrae[id.xy] = penumbra;
	translucencies[id.xy] = SIGMA_FrontEnd_PackTranslucency( penumbra, ratio.rgb );
}
