// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	One direction of a bloom level's Gaussian. A group blurs LINE texels of a row (or of a column, vertical) from
	the LINE + 2 * radius around them in groupshared, the level's edge texels repeated beyond it.

	The vertical pass also multiplies by the level's tint and adds the next smaller level's sum, upsampled
	bilinearly: from the smallest level up, the first one ends up holding the tinted sum of all of them.
*/

#define LINE 128
#define MAX_RADIUS 64

cbuffer BlurParms : register( b0 ) {
	uint sourceSrv;
	uint targetUav;			// RGBA16F
	uint smallerSrv;		// the next smaller level's sum; NO_SMALLER for the smallest
	uint linearSampler;
	uint2 size;				// of the level
	uint vertical;
	uint radius;			// taps on each side, at most MAX_RADIUS
	float3 tint;			// vertical
	float smallerScaleX;	// uv in the smaller level of a texel: ( texel + 0.5 ) * scale
	float smallerScaleY;
	float3 unused;
	float4 weights[17];		// the tap at distance t is weights[t / 4][t % 4]; with the mirrored taps they sum to 1
};

static const uint NO_SMALLER = 0xFFFFFFFF;

groupshared float3 texels[LINE + 2 * MAX_RADIUS];

[numthreads( LINE, 1, 1 )]
void CSMain( uint3 group : SV_GroupID, uint index : SV_GroupIndex ) {
	Texture2D<float4> source = ResourceDescriptorHeap[sourceSrv];
	const uint count = ( vertical != 0 ) ? size.y : size.x;
	const uint across = group.y;
	const int first = int( group.x * LINE ) - int( radius );
	for ( uint i = index; i < LINE + 2 * radius; i += LINE ) {
		const uint along = (uint)clamp( first + int( i ), 0, int( count ) - 1 );
		texels[i] = source.Load( int3( ( vertical != 0 ) ? uint2( across, along ) : uint2( along, across ), 0 ) ).rgb;
	}
	GroupMemoryBarrierWithGroupSync();
	const uint along = group.x * LINE + index;
	if ( along >= count ) {
		return;
	}
	float3 sum = texels[index + radius] * weights[0].x;
	for ( uint t = 1; t <= radius; t++ ) {
		sum += ( texels[index + radius - t] + texels[index + radius + t] ) * weights[t / 4][t % 4];
	}
	const uint2 texel = ( vertical != 0 ) ? uint2( across, along ) : uint2( along, across );
	if ( vertical != 0 ) {
		sum *= tint;
		if ( smallerSrv != NO_SMALLER ) {
			Texture2D<float4> smaller = ResourceDescriptorHeap[smallerSrv];
			SamplerState filter = SamplerDescriptorHeap[linearSampler];
			sum += smaller.SampleLevel( filter, ( float2( texel ) + 0.5 ) * float2( smallerScaleX, smallerScaleY ), 0 ).rgb;
		}
	}
	RWTexture2D<float4> target = ResourceDescriptorHeap[targetUav];
	target[texel] = float4( sum, 1.0 );
}
