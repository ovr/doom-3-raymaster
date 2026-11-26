// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The traced shadows into the HDR scene's viewport: the blend multiplies the colour by what internal/rtShadowRatio.hlsl
// leaves of it. r_rtDebug 6 puts that in the colour's place, blue where no traced light reaches. The write mask
// keeps the scene's alpha.

cbuffer ApplyParms : register( b1 ) {
	int2 first;				// the viewport's first pixel in the scene
	uint ratioSrv;
	float show;				// 0, or preExposure: the picture in the scene's place
};

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	Texture2D<float4> ratio = ResourceDescriptorHeap[ratioSrv];
	const float4 r = ratio.Load( int3( int2( position.xy ) - first, 0 ) );
	const float3 left = saturate( r.rgb );
	if ( show > 0.0 ) {
		return float4( ( ( r.a > 0.0 ) ? left : float3( 0.0, 0.0, 0.25 ) ) * show, 0.0 );
	}
	return float4( left, 0.0 );
}
