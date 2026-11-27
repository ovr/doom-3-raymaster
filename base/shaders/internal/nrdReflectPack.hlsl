// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The glossy reflections' rays (internal/rtReflect.hlsl, rows from the bottom) into NRD's IN_SPEC_RADIANCE_HITDIST
	of REBLUR_SPECULAR, rows from the top: the radiance and how far the ray went, normalized as REBLUR wants it
	(REBLUR_FrontEnd_GetNormHitDist with the same hitDistParams as the denoiser's settings). A pixel that is no
	glossy one (material 0 of the guides: a mirror, one that reflects nothing) sends nothing.
*/

#include "../include/rtReflection.hlsli"
#include "../include/nrd/NRD.hlsli"

cbuffer PackParms : register( b0 ) {
	float4 hitDistParams;		// xyz ReblurHitDistanceParameters' A, B, C
	uint2 size;					// texels
	uint raysSrv;
	uint viewZSrv;
	uint normalRoughnessSrv;
	uint outputUav;
	uint unused0;
	uint unused1;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	Texture2D<float4> rays = ResourceDescriptorHeap[raysSrv];
	Texture2D<float> viewZs = ResourceDescriptorHeap[viewZSrv];
	Texture2D<float4> normalRoughness = ResourceDescriptorHeap[normalRoughnessSrv];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
	float materialId;
	const float4 nr = NRD_FrontEnd_UnpackNormalAndRoughness( normalRoughness.Load( int3( id.xy, 0 ) ), materialId );
	const float4 ray = rays.Load( int3( id.x, size.y - 1 - id.y, 0 ) );
	if ( materialId < 0.5 || ray.a < 0.0 ) {
		output[id.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	const float hitDist = REBLUR_FrontEnd_GetNormHitDist( ray.a, viewZs.Load( int3( id.xy, 0 ) ), hitDistParams.xyz, nr.w );
	output[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist( ray.rgb, hitDist, true );
}
