// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	R15: the vertices of one skinned geometry as the skinned programs draw them, in idDrawVert's layout, for its
	acceleration structure and the hits on it. The joints are three float4 rows each, as the joint cache holds them;
	the weighted sum and its order are skinning.hlsli's, the weights those of idDrawVert::GetSkinnedDrawVert. The
	normal and the tangent are normalized and encoded as the vertex cache encodes them; the rest is copied.
*/

#include "../include/raytracing.hlsli"

cbuffer SkinParms : register( b1 ) {
	uint sourceSrv;			// the static vertex buffer, raw
	uint sourceOffset;		// bytes of the geometry's first vertex
	uint numVerts;
	uint jointSrv;			// the frame's joint buffer, raw
	uint jointOffset;		// bytes of the instance's first joint
	uint targetUav;			// the skinned vertices, raw
	uint targetOffset;		// bytes of the geometry's first skinned vertex
	uint unused;
};

// VERTEX_FLOAT_TO_BYTE per component; the fourth byte (the tangent's sign) stays.
uint EncodeDirection( float3 direction, uint packed ) {
	const uint3 bytes = (uint3)clamp( floor( ( direction + 1.0 ) * 127.5 + 0.5 ), 0.0, 255.0 );
	return bytes.x | ( bytes.y << 8 ) | ( bytes.z << 16 ) | ( packed & 0xFF000000 );
}

[numthreads( 64, 1, 1 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( id.x >= numVerts ) {
		return;
	}
	ByteAddressBuffer source = ResourceDescriptorHeap[sourceSrv];
	ByteAddressBuffer joints = ResourceDescriptorHeap[jointSrv];
	RWByteAddressBuffer target = ResourceDescriptorHeap[targetUav];
	const uint4 head = source.Load4( sourceOffset + id.x * 32 );		// xyz, st
	const uint4 tail = source.Load4( sourceOffset + id.x * 32 + 16 );	// normal, tangent, color (joints), color2 (weights)

	float4 rows[3] = { float4( 0.0, 0.0, 0.0, 0.0 ), float4( 0.0, 0.0, 0.0, 0.0 ), float4( 0.0, 0.0, 0.0, 0.0 ) };
	[unroll]
	for ( uint j = 0; j < 4; j++ ) {
		const uint joint = ( tail.z >> ( j * 8 ) ) & 0xFF;
		const float weight = float( ( tail.w >> ( j * 8 ) ) & 0xFF ) * ( 1.0 / 255.0 );
		[unroll]
		for ( uint r = 0; r < 3; r++ ) {
			rows[r] += asfloat( joints.Load4( jointOffset + ( joint * 3 + r ) * 16 ) ) * weight;
		}
	}
	const float4 position = float4( asfloat( head.xyz ), 1.0 );
	const float3 skinned = float3( dot( rows[0], position ), dot( rows[1], position ), dot( rows[2], position ) );
	const float3 normal = RtDecodeDirection( tail.x );
	const float3 tangent = RtDecodeDirection( tail.y );
	const float3 skinnedNormal = normalize( float3( dot( rows[0].xyz, normal ), dot( rows[1].xyz, normal ), dot( rows[2].xyz, normal ) ) );
	const float3 skinnedTangent = normalize( float3( dot( rows[0].xyz, tangent ), dot( rows[1].xyz, tangent ), dot( rows[2].xyz, tangent ) ) );

	const uint at = targetOffset + id.x * 32;
	target.Store4( at, uint4( asuint( skinned ), head.w ) );
	target.Store4( at + 16, uint4( EncodeDirection( skinnedNormal, tail.x ), EncodeDirection( skinnedTangent, tail.y ), tail.z, tail.w ) );
}
