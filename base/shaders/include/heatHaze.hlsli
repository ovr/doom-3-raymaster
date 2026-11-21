// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

// The varyings of heatHazeWithMask and heatHazeWithMaskAndVertex: GL also links the VS of the second with the PS
// of the first, and D3D12 links by register, so both files declare this one struct. Only the second writes color.
struct VSOutput {
	float4 position : SV_Position;
	float4 texcoord0 : TEXCOORD0;
	float4 texcoord1 : TEXCOORD1;
	float4 texcoord2 : TEXCOORD2;
	float4 color : COLOR0;
};
