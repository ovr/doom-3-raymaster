// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// r_gbufferDebug: one target of the G-buffer (include/gbuffer.hlsli) as display colour, sample 0 under MSAA.

#include "../include/gbuffer.hlsli"
#include "../include/srgb.hlsli"

cbuffer DebugParms : register( b1 ) {
	uint normalSrv;
	uint materialSrv;
	uint motionSrv;
	uint albedoSrv;
	uint mode;			// gbufferDebug_t: 1 normal, 2 F0, 3 roughness, 4 motion, 5 albedo
};

static const float MOTION_FULL = 8.0;	// pixels of motion at full brightness

#if MULTISAMPLED
float4 Fetch( uint srv, int2 p ) {
	Texture2DMS<float4> target = ResourceDescriptorHeap[srv];
	return target.Load( p, 0 );
}
#else
float4 Fetch( uint srv, int2 p ) {
	Texture2D<float4> target = ResourceDescriptorHeap[srv];
	return target.Load( int3( p, 0 ) );
}
#endif

float3 Hue( float h ) {
	const float3 rgb = abs( frac( h + float3( 0.0, 2.0 / 3.0, 1.0 / 3.0 ) ) * 6.0 - 3.0 ) - 1.0;
	return saturate( rgb );
}

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	const int2 p = int2( position.xy );
	const float2 encoded = Fetch( normalSrv, p ).xy;
	const bool surface = any( encoded != 0.0 );
	float3 c = float3( 0.0, 0.0, 0.0 );
	if ( mode == 1 ) {
		c = surface ? 0.5 + 0.5 * DecodeOctahedral( encoded ) : c;
	} else if ( mode == 2 ) {
		c = LinearToSRGB( Fetch( materialSrv, p ).rgb );
	} else if ( mode == 3 ) {
		c = surface ? Fetch( materialSrv, p ).aaa : c;
	} else if ( mode == 4 ) {
		const float2 motion = Fetch( motionSrv, p ).xy;
		const float pixels = sqrt( dot( motion, motion ) );
		const float angle = ( pixels > 0.0 ) ? atan2( motion.y, motion.x ) : 0.0;
		c = Hue( angle / 6.28318530718 + 0.5 ) * saturate( pixels / MOTION_FULL );
	} else if ( mode == 5 ) {
		c = LinearToSRGB( Fetch( albedoSrv, p ).rgb );
	}
	return float4( c, 1.0 );
}
