// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The bilateral grid of the local exposure. A group is a 64 x 64 pixel tile of the viewport: the log2
	luminance of each of its 2 x 2 blocks, clamped to minLog .. minLog + logRange, goes into one of GRID_BINS
	bins, which keep the sum (relative to minLog) and the count. The sums are fixed point in groupshared
	integers, so the order of the atomics changes nothing. The tile's totals over every bin go to their own
	texture, the source of the blurred luminance.
*/

#include "../include/exposure.hlsli"

#define TILE 64
#define GRID_BINS 32
#define FIXED 256.0

cbuffer GridParms : register( b0 ) {
	uint sceneSrv;			// RGBA16F, one sample
	uint gridUav;			// Texture3D RG32F: tiles x, tiles y, GRID_BINS
	uint totalsUav;			// Texture2D RG32F: tiles x, tiles y
	uint x0;				// the viewport
	uint y0;
	uint width;
	uint height;
	float invPreExposure;
	float minLog;			// log2 luminance where bin 0 begins
	float binsPerLog;
	float logRange;
};

groupshared uint sums[GRID_BINS];
groupshared uint counts[GRID_BINS];

[numthreads( 16, 16, 1 )]
void CSMain( uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex ) {
	if ( index < GRID_BINS ) {
		sums[index] = 0;
		counts[index] = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	Texture2D<float4> scene = ResourceDescriptorHeap[sceneSrv];
	const int2 last = int2( x0 + width - 1, y0 + height - 1 );
	for ( uint b = 0; b < 4; b++ ) {
		const uint2 local = group.xy * TILE + ( thread.xy * 2 + uint2( b & 1, b >> 1 ) ) * 2;	// of the viewport
		if ( any( local >= uint2( width, height ) ) ) {
			continue;
		}
		const int2 origin = int2( x0, y0 ) + int2( local );
		float3 sum = 0.0;
		for ( uint i = 0; i < 4; i++ ) {
			sum += scene.Load( int3( min( origin + int2( i & 1, i >> 1 ), last ), 0 ) ).rgb;
		}
		const float luminance = dot( sum, EXPOSURE_LUMA ) * 0.25 * invPreExposure;
		const float relative = clamp( log2( max( luminance, 1e-30 ) ) - minLog, 0.0, logRange );	// max( NaN, x ) is x
		const uint bin = min( (uint)( relative * binsPerLog ), GRID_BINS - 1 );
		InterlockedAdd( sums[bin], (uint)( relative * FIXED + 0.5 ) );
		InterlockedAdd( counts[bin], 1 );
	}
	GroupMemoryBarrierWithGroupSync();
	if ( index < GRID_BINS ) {
		RWTexture3D<float2> grid = ResourceDescriptorHeap[gridUav];
		grid[uint3( group.xy, index )] = float2( float( sums[index] ) / FIXED, float( counts[index] ) );
	}
	if ( index == 0 ) {
		uint sum = 0;
		uint count = 0;
		for ( uint i = 0; i < GRID_BINS; i++ ) {
			sum += sums[i];
			count += counts[i];
		}
		RWTexture2D<float2> totals = ResourceDescriptorHeap[totalsUav];
		totals[group.xy] = float2( float( sum ) / FIXED, float( count ) );
	}
}
