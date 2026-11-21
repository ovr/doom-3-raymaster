// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "common.hlsli"

// CenterScale and Rotate2D of global.inc: a texture coordinate scaled or turned around ( 0.5, 0.5 ).
float2 CenterScale( float2 inTC, float2 centerScale ) {
	const float scaleX = centerScale.x;
	const float scaleY = centerScale.y;
	const float4 tc0 = float4( scaleX, 0, 0, 0.5 - ( 0.5 * scaleX ) );
	const float4 tc1 = float4( 0, scaleY, 0, 0.5 - ( 0.5 * scaleY ) );

	float2 finalTC;
	finalTC.x = dot4( inTC, tc0 );
	finalTC.y = dot4( inTC, tc1 );
	return finalTC;
}

// cs is ( cos, sin ) of the angle.
float2 Rotate2D( float2 inTC, float2 cs ) {
	const float sinValue = cs.y;
	const float cosValue = cs.x;

	const float4 tc0 = float4( cosValue, -sinValue, 0, ( -0.5 * cosValue ) + ( 0.5 * sinValue ) + 0.5 );
	const float4 tc1 = float4( sinValue, cosValue, 0, ( -0.5 * sinValue ) + ( -0.5 * cosValue ) + 0.5 );

	float2 finalTC;
	finalTC.x = dot4( inTC, tc0 );
	finalTC.y = dot4( inTC, tc1 );
	return finalTC;
}
