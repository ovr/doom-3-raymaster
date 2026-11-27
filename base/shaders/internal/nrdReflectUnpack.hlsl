// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	REBLUR_SPECULAR's OUT_SPEC_RADIANCE_HITDIST (rows from the top) back into what internal/rtComposite.hlsl reads
	(rows from the bottom, as the rays): a glossy pixel's filtered radiance, a mirror's ray as it is. Without the
	frame's rays (flags bit 0: a frame that traces nothing) a glossy pixel takes NRD's last output and a mirror
	nothing.
*/

#include "../include/rtReflection.hlsli"
#include "../include/nrd/NRD.hlsli"

cbuffer UnpackParms : register( b0 ) {
	uint2 size;					// texels
	uint denoisedSrv;
	uint raysSrv;
	uint normalRoughnessSrv;
	uint outputUav;
	uint flags;					// bit 0: the frame's rays are there
	uint unused;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	Texture2D<float4> denoised = ResourceDescriptorHeap[denoisedSrv];
	Texture2D<float4> normalRoughness = ResourceDescriptorHeap[normalRoughnessSrv];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
	const uint2 nrdTexel = uint2( id.x, size.y - 1 - id.y );
	float materialId;
	NRD_FrontEnd_UnpackNormalAndRoughness( normalRoughness.Load( int3( nrdTexel, 0 ) ), materialId );
	float3 radiance = float3( 0.0, 0.0, 0.0 );
	if ( materialId >= 0.5 ) {
		radiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist( denoised.Load( int3( nrdTexel, 0 ) ) ).rgb;
	} else if ( ( flags & 1u ) != 0 ) {
		Texture2D<float4> rays = ResourceDescriptorHeap[raysSrv];
		const float4 ray = rays.Load( int3( id.xy, 0 ) );
		radiance = ( ray.a == RT_REFLECT_MIRROR ) ? ray.rgb : float3( 0.0, 0.0, 0.0 );
	}
	output[id.xy] = float4( max( radiance, 0.0 ), 0.0 );
}
