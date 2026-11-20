// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A quad already in clip space at GL's near plane that writes depth 1 and colour 0 (renderprogs/zcullReconstruct).

#include "include/common.hlsli"

struct VSInput {
	float4 position : POSITION;
};

struct VSOutput {
	float4 position : SV_Position;
};

struct PSOutput {
	float4 color : SV_Target0;
	float depth : SV_Depth;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
	float4 glPosition = input.position;
	glPosition.z = -1.0;
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;
	return output;
}

PSOutput PSMain( VSOutput input ) {
	PSOutput output;
	output.depth = 1;
	output.color = float4( 0.0, 0.0, 0.0, 0.0 );
	return output;
}
