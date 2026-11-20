// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// Scene → back buffer. Memory row 0 of the scene is the bottom of the screen, row 0 of a back buffer its top.
// GAMMA=1 looks each channel's byte up in its channel of the LUT; alpha stays, the swap chain ignores it.
cbuffer BlitParms : register( b1 ) {
	int4 sceneSize;		// x = width, y = height
};

Texture2D<float4> scene : register( t0 );
#if GAMMA
Texture2D<float4> gammaLut : register( t1 );
#endif

float4 PSMain( float4 position : SV_Position ) : SV_Target {
	int2 pixel = int2( position.xy );
	float4 texel = scene.Load( int3( pixel.x, sceneSize.y - 1 - pixel.y, 0 ) );
#if GAMMA
	int3 index = int3( texel.rgb * 255.0 + 0.5 );
	texel.rgb = float3( gammaLut.Load( int3( index.r, 0, 0 ) ).r, gammaLut.Load( int3( index.g, 0, 0 ) ).g, gammaLut.Load( int3( index.b, 0, 0 ) ).b );
#endif
	return texel;
}
