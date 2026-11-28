// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The glossy reflections' rays (internal/rtReflect.hlsl, rows from the bottom) into NRD's IN_SPEC_RADIANCE_HITDIST
	of REBLUR_SPECULAR, rows from the top: the radiance and how far the ray went, normalized as REBLUR wants it
	(REBLUR_FrontEnd_GetNormHitDist with the same hitDistParams as the denoiser's settings). A pixel that is no
	glossy one (material 0 of the guides: a mirror, one that reflects nothing) sends nothing.
	Under REBLUR's WHITE checkerboard (flags bit 0) a texel per two pixels of a row: the one whose x + row + frame
	is even (internal/rtReflect.hlsl traced it), packed into the left half as NRD wants.
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
	uint flags;					// bit 0: the checkerboard
	uint frameIndex;			// NRD's
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	const bool checkerboard = ( flags & 1u ) != 0;
	const uint2 pixel = checkerboard ? uint2( id.x * 2 + ( ( id.y + frameIndex ) & 1u ), id.y ) : id.xy;
	if ( any( id.xy >= size ) ) {
		return;
	}
	Texture2D<float4> rays = ResourceDescriptorHeap[raysSrv];
	Texture2D<float> viewZs = ResourceDescriptorHeap[viewZSrv];
	Texture2D<float4> normalRoughness = ResourceDescriptorHeap[normalRoughnessSrv];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
	float materialId;
	if ( pixel.x >= size.x ) {
		output[id.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	const float4 nr = NRD_FrontEnd_UnpackNormalAndRoughness( normalRoughness.Load( int3( pixel, 0 ) ), materialId );
	const float4 ray = rays.Load( int3( pixel.x, size.y - 1 - pixel.y, 0 ) );
	if ( materialId < 0.5 || ray.a < 0.0 ) {
		output[id.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	const float hitDist = REBLUR_FrontEnd_GetNormHitDist( ray.a, viewZs.Load( int3( pixel, 0 ) ), hitDistParams.xyz, nr.w );
	output[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist( ray.rgb, hitDist, true );
}
