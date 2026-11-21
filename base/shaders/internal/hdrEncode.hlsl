// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The HDR scene between linear and its sRGB encoding (the gamma composite of include/colorspace.hlsli), from a
// copy of the scene; under MSAA once per sample. The curve goes on above 1, so the round trip keeps the HDR.

#include "../include/srgb.hlsli"

cbuffer EncodeParms : register( b1 ) {
	uint sourceSrv;		// the copy, RGBA16F
	uint toGamma;		// 1: linear → sRGB encoding, 0: back
};

#if MULTISAMPLED
float4 PSMain( float4 position : SV_Position, uint sampleIndex : SV_SampleIndex ) : SV_Target {
	Texture2DMS<float4> source = ResourceDescriptorHeap[sourceSrv];
	float4 c = source.Load( int2( position.xy ), sampleIndex );
#else
float4 PSMain( float4 position : SV_Position ) : SV_Target {
	Texture2D<float4> source = ResourceDescriptorHeap[sourceSrv];
	float4 c = source.Load( int3( position.xy, 0 ) );
#endif
	c.rgb = ( toGamma != 0 ) ? LinearToSRGB( max( c.rgb, 0.0 ) ) : SRGBToLinear( c.rgb );
	return c;
}
