// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The vertex stage of the backend's own passes: (-1, 1), (3, 1), (-1, -3), one triangle over the viewport
// that the scissor cuts.
float4 VSMain( uint vertexId : SV_VertexID ) : SV_Position {
	float2 corner = float2( ( vertexId << 1 ) & 2, vertexId & 2 );
	return float4( corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0 );
}
