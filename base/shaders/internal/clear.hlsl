// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// A clear through the colour write mask, which ClearRenderTargetView ignores.
cbuffer ClearParms : register( b1 ) {
	float4 clearColor;
};

float4 PSMain() : SV_Target {
	return clearColor;
}
