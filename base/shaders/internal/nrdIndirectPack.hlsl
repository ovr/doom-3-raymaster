// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The indirect light's rays (internal/rtIndirect.hlsl, a texel of the grid each, rows from the bottom) into NRD's
	IN_DIFF_SH0 and IN_DIFF_SH1 of REBLUR_DIFFUSE_SH, rows from the top. The rays keep what REBLUR_FrontEnd_PackSh
	lays out (include/rtBounce.hlsli: the luminance and chroma of YCoCg, the luminance times the direction), so they
	go as they are, with how far the ray went (the chroma's b) normalized as REBLUR wants it
	(REBLUR_FrontEnd_GetNormHitDist at roughness 1, the hitDistParams of the denoiser's settings).
*/

#include "../include/nrd/NRD.hlsli"

cbuffer PackParms : register( b0 ) {
	float4 hitDistParams;		// xyz ReblurHitDistanceParameters' A, B, C
	uint2 size;					// texels
	uint amountSrv;
	uint chromaSrv;
	uint viewZSrv;
	uint unused0;
	uint sh0Uav;
	uint sh1Uav;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	Texture2D<float4> amounts = ResourceDescriptorHeap[amountSrv];
	Texture2D<float4> chromas = ResourceDescriptorHeap[chromaSrv];
	Texture2D<float> viewZs = ResourceDescriptorHeap[viewZSrv];
	RWTexture2D<float4> sh0 = ResourceDescriptorHeap[sh0Uav];
	RWTexture2D<float4> sh1 = ResourceDescriptorHeap[sh1Uav];
	const int3 ray = int3( id.x, size.y - 1 - id.y, 0 );
	float4 amount = amounts.Load( ray );
	const float4 coded = chromas.Load( ray );
	float2 chroma = coded.rg;
	if ( any( isnan( amount ) ) || any( isinf( amount ) ) || any( isnan( chroma ) ) || any( isinf( chroma ) ) ) {
		amount = float4( 0.0, 0.0, 0.0, 0.0 );
		chroma = float2( 0.0, 0.0 );
	}
	const float hitDist = REBLUR_FrontEnd_GetNormHitDist( coded.b, viewZs.Load( int3( id.xy, 0 ) ), hitDistParams.xyz, 1.0 );
	sh0[id.xy] = float4( max( amount.a, 0.0 ), chroma, hitDist );
	sh1[id.xy] = float4( amount.xyz, 0.0 );
}
