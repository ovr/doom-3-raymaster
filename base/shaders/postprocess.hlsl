// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The resolved view copied to the screen by a quad already in clip space (renderprogs/postprocess).

#include "include/common.hlsli"

TEXTURE2D( Texture2D, viewColorMap, 0 );
SAMPLER( viewColorSampler, 0 );

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
	const float4 glPosition = input.position;
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;
	output.texcoord = input.texcoord;
	return output;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float2 tCoords = input.texcoord;
	return viewColorMap.Sample( viewColorSampler, tCoords );
}
