// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "common.hlsli"

// Three rows of a 3x4 matrix per joint. A vertex names up to four joints in COLOR0 and weighs them in COLOR1.
// gbuffer_skinned reads a second block of joints after the first (R14), so it declares twice the rows.
#ifndef JOINT_VECTORS
#define JOINT_VECTORS 408
#endif
cbuffer Joints : register( b2 ) {
	float4 matrices[JOINT_VECTORS];
};

struct SkinMatrix {
	float4 x;
	float4 y;
	float4 z;
};

// The weighted sum of a vertex's joints, in the order and with the rounding of skinning.inc:
// multiplying by 255.1 gives the same index as floor( c * 255 + 0.5 ) and is faster.
// base is the row of the block's first joint.
SkinMatrix SkinningMatrixAt( float4 joints, float4 weights, int base ) {
	SkinMatrix m;
	float joint = joints.x * 255.1 * 3;
	m.x = matrices[base + int( joint + 0 )] * weights.x;
	m.y = matrices[base + int( joint + 1 )] * weights.x;
	m.z = matrices[base + int( joint + 2 )] * weights.x;

	joint = joints.y * 255.1 * 3;
	m.x += matrices[base + int( joint + 0 )] * weights.y;
	m.y += matrices[base + int( joint + 1 )] * weights.y;
	m.z += matrices[base + int( joint + 2 )] * weights.y;

	joint = joints.z * 255.1 * 3;
	m.x += matrices[base + int( joint + 0 )] * weights.z;
	m.y += matrices[base + int( joint + 1 )] * weights.z;
	m.z += matrices[base + int( joint + 2 )] * weights.z;

	joint = joints.w * 255.1 * 3;
	m.x += matrices[base + int( joint + 0 )] * weights.w;
	m.y += matrices[base + int( joint + 1 )] * weights.w;
	m.z += matrices[base + int( joint + 2 )] * weights.w;
	return m;
}

SkinMatrix SkinningMatrix( float4 joints, float4 weights ) {
	return SkinningMatrixAt( joints, weights, 0 );
}

float4 SkinPosition( SkinMatrix m, float4 position ) {
	return float4( dot4( m.x, position ), dot4( m.y, position ), dot4( m.z, position ), 1.0 );
}

// A direction (normal, tangent, binormal) through the matrix, normalized.
float3 SkinDirection( SkinMatrix m, float3 direction ) {
	return normalize( float3( dot3( m.x, direction ), dot3( m.y, direction ), dot3( m.z, direction ) ) );
}
