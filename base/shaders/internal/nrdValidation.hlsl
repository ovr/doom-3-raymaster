// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	r_rtNrdValidation: NRD's OUT_VALIDATION (rows from the top, the viewport's size) over the output, by its alpha.
	Viewport 3 of it is black where the motion and the matrices agree for a still world.
*/

cbuffer ValidationParms : register( b1 ) {
	int2 first;					// the viewport's first pixel in the output
	uint2 size;					// texels
	uint validationSrv;
	uint unused0;
	uint unused1;
	uint unused2;
};

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	Texture2D<float4> validation = ResourceDescriptorHeap[validationSrv];
	const int2 texel = int2( position.xy ) - first;
	return validation.Load( int3( texel.x, int( size.y ) - 1 - texel.y, 0 ) );
}
