// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	r_rtReference (R18-R20): a traced signal's plain mean over the frames of an eye that stands still, in RGBA32F (a
	half float stops counting at 2048): the mean before plus what this frame adds, weight 1 over the frames the new
	mean holds. flags bit 0: the mean before is there (else this frame is the mean); bit 1: no rays, the mean stays.
*/

cbuffer AccumulateParms : register( b0 ) {
	uint2 size;					// texels
	uint signalSrv;
	uint meanSrv;				// the mean before
	uint meanUav;
	float weight;
	uint flags;
	uint unused0;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( any( id.xy >= size ) ) {
		return;
	}
	const int3 at = int3( id.xy, 0 );
	float4 mean = float4( 0.0, 0.0, 0.0, 0.0 );
	if ( ( flags & 1u ) != 0 ) {
		Texture2D<float4> means = ResourceDescriptorHeap[meanSrv];
		mean = means.Load( at );
	}
	if ( ( flags & 2u ) == 0 ) {
		Texture2D<float4> signals = ResourceDescriptorHeap[signalSrv];
		const float4 value = signals.Load( at );
		mean = ( ( flags & 1u ) != 0 ) ? mean + ( value - mean ) * weight : value;
	}
	RWTexture2D<float4> output = ResourceDescriptorHeap[meanUav];
	output[id.xy] = mean;
}
