// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	NRD's OUT_DIFF_SH0 and OUT_DIFF_SH1 of the indirect light's grid (rows from the top) back into what
	internal/rtUpsample.hlsl reads of the rays (include/rtBounce.hlsli, rows from the bottom): the amount, and
	the chroma with a ray in aux.a; a texel beyond NRD's denoising range (nothing drawn) holds nothing.
*/

cbuffer UnpackParms : register( b0 ) {
	uint2 size;					// texels
	uint sh0Srv;
	uint sh1Srv;
	uint viewZSrv;
	uint amountUav;
	uint auxUav;
	float range;				// NRD's denoising range
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	Texture2D<float4> sh0 = ResourceDescriptorHeap[sh0Srv];
	Texture2D<float4> sh1 = ResourceDescriptorHeap[sh1Srv];
	Texture2D<float> viewZs = ResourceDescriptorHeap[viewZSrv];
	RWTexture2D<float4> amount = ResourceDescriptorHeap[amountUav];
	RWTexture2D<float4> aux = ResourceDescriptorHeap[auxUav];
	const int3 texel = int3( id.x, size.y - 1 - id.y, 0 );
	if ( !( viewZs.Load( texel ) < range ) ) {
		amount[id.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		aux[id.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	const float4 c0 = sh0.Load( texel );
	amount[id.xy] = float4( sh1.Load( texel ).xyz, c0.x );
	aux[id.xy] = float4( c0.yz, 0.0, 1.0 );
}
