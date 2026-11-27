// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	NRD's OUT_SHADOW_TRANSLUCENCY of SIGMA_SHADOW_TRANSLUCENCY (rows from the top) back into what
	internal/rtShadowApply.hlsl reads (rows from the bottom): its yzw, what the shadows leave of the lights.
*/

#include "../include/nrd/NRD.hlsli"

cbuffer UnpackParms : register( b0 ) {
	uint2 size;					// pixels
	uint shadowSrv;
	uint outputUav;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	Texture2D<float4> shadows = ResourceDescriptorHeap[shadowSrv];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
	const float4 shadow = SIGMA_BackEnd_UnpackShadow( shadows.Load( int3( id.x, size.y - 1 - id.y, 0 ) ) );
	output[id.xy] = float4( shadow.yzw, 1.0 );
}
