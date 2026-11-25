// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The reflections of internal/rtReflect.hlsl into the HDR scene's viewport: radiance times the scale and preExposure,
// which the blend adds (or puts in place of the colour, r_rtDebug 5). The write mask keeps the scene's alpha.

#include "../include/srgb.hlsli"

cbuffer CompositeParms : register( b1 ) {
	int2 first;				// the viewport's first pixel in the scene
	float scale;			// r_rtReflectionScale times preExposure
	uint reflectionSrv;
};

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	Texture2D<float4> reflection = ResourceDescriptorHeap[reflectionSrv];
	const float3 c = reflection.Load( int3( int2( position.xy ) - first, 0 ) ).rgb * scale;
	return float4( min( max( c, 0.0 ), HALF_MAX ), 0.0 );
}
