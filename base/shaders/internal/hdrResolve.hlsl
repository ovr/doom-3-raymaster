// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The MSAA resolve of the HDR scene before the tonemap: the samples weighted by 1 / ( 1 + luma ) (Karis), so a
// bright sample beside dark ones does not paint a hard step across an edge, which a plain average would after
// the curve. The weights see the luma the tonemapper sees.

cbuffer ResolveParms : register( b1 ) {
	uint sceneSrv;		// Texture2DMS RGBA16F
	uint outputUav;		// RWTexture2D RGBA16F
	uint samples;
	float lumaScale;	// exposure / preExposure
	uint x0;			// the viewport
	uint y0;
	uint width;
	uint height;
};

[numthreads( 8, 8, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( id.x >= width || id.y >= height ) {
		return;
	}
	Texture2DMS<float4> scene = ResourceDescriptorHeap[sceneSrv];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];
	const int2 pixel = int2( x0 + id.x, y0 + id.y );
	float3 sum = 0.0;
	float weights = 0.0;
	float alpha = 0.0;
	for ( uint s = 0; s < samples; s++ ) {
		const float4 c = scene.Load( pixel, s );
		const float w = 1.0 / ( 1.0 + max( dot( c.rgb, float3( 0.2126, 0.7152, 0.0722 ) ) * lumaScale, 0.0 ) );
		sum += c.rgb * w;
		weights += w;
		alpha += c.a;
	}
	output[pixel] = float4( sum / weights, alpha / samples );
}
