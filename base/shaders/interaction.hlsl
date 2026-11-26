// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// One light on a surface: bump, diffuse and specular maps, the light's projection and falloff (renderprogs/interaction).
// SKINNED=1 is interaction_skinned. The parms, VSMain and the colour are include/interaction.hlsli.

#include "include/interaction.hlsli"

float4 PSMain( VSOutput input ) : SV_Target {
	return SceneOutput( InteractionColor( input ) );
}
