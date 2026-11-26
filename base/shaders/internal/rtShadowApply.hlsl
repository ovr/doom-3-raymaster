// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The traced shadows into the HDR scene's viewport: the blend multiplies the colour by what is left of it, as
// internal/rtShadowRatio.hlsl found it or as the filter left it (R18). r_rtDebug 6 puts that in the colour's place,
// blue where no traced light reaches; r_rtDebug 7 the length of the history, black without one. The write mask keeps
// the scene's alpha.

cbuffer ApplyParms : register( b1 ) {
	int2 first;				// the viewport's first pixel in the scene
	uint ratioSrv;			// rgb what is left
	float show;				// 0, or preExposure: a picture in the scene's place
	uint reachSrv;			// a the unshadowed luminance: internal/rtShadowRatio.hlsl's
	uint lengthSrv;			// a the frames of the history
	float lengthScale;		// 1 over the frames a history may hold, 0 without one
	uint lengthView;		// 1: the picture is the history's length
};

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	Texture2D<float4> ratio = ResourceDescriptorHeap[ratioSrv];
	const int3 at = int3( int2( position.xy ) - first, 0 );
	const float3 left = saturate( ratio.Load( at ).rgb );
	if ( show > 0.0 ) {
		if ( lengthView != 0 ) {
			Texture2D<float4> history = ResourceDescriptorHeap[lengthSrv];
			const float held = saturate( history.Load( at ).a * lengthScale );
			return float4( float3( held, held, held ) * show, 0.0 );
		}
		Texture2D<float4> reach = ResourceDescriptorHeap[reachSrv];
		return float4( ( ( reach.Load( at ).a > 0.0 ) ? left : float3( 0.0, 0.0, 0.25 ) ) * show, 0.0 );
	}
	return float4( left, 0.0 );
}
