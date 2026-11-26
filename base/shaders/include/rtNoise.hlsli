// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	Two numbers in [0, 1) per pixel and index, for one sample per pixel: the blue noise of renderer/BlueNoise.h
	(a texture of 64 x 64 that repeats, its two channels in .xw) shifted by the R2 sequence of the index.
	Neighbouring pixels get values far apart, so the error of one sample is of high frequencies, which a
	filter of a few pixels removes; a pixel's values over the indices cover the square evenly, so its history
	converges faster than with random numbers. The same pixel and index give the same numbers in every run.

	A ray that needs several pairs (its direction, a light, a point of the light's disk) takes each with a step of
	its own. With one step the pairs move together: a pixel's rays fill a plane of their six dimensions, and what
	depends on two pairs at once (which light, among the rays that hit) keeps an error at any number of frames.
*/

static const uint RT_NOISE_SIZE = 64;
static const uint RT_NOISE_PAIRS = 3;

// Per pair the step of an index as 32-bit fractions, exact in integers at any index: 1 / p and 1 / p^2 of the
// plastic number, then the square roots of 2 and 3, of 5 and 7.
static const uint2 RT_NOISE_STEPS[RT_NOISE_PAIRS] = {
	uint2( 3242174889u, 2447445414u ), uint2( 1779033703u, 3144134277u ), uint2( 1013904242u, 2773480762u )
};

float2 RtNoise( uint noiseSrv, uint2 pixel, uint index, uint pair ) {
	Texture2D<float4> noise = ResourceDescriptorHeap[noiseSrv];
	const float2 base = ( noise.Load( int3( pixel & ( RT_NOISE_SIZE - 1 ), 0 ) ).xw * 255.0 + 0.5 ) / 256.0;
	const uint2 shift = index * RT_NOISE_STEPS[pair];
	return frac( base + float2( shift >> 8 ) * ( 1.0 / 16777216.0 ) );
}

float2 RtNoise( uint noiseSrv, uint2 pixel, uint index ) {
	return RtNoise( noiseSrv, pixel, index, 0 );
}

// The unit square onto the unit disk, concentric: areas and neighbourhoods are kept, so is the noise's spectrum.
float2 RtDisk( float2 u ) {
	const float2 p = u * 2.0 - 1.0;
	if ( p.x == 0.0 && p.y == 0.0 ) {
		return float2( 0.0, 0.0 );
	}
	const float quarter = 0.78539816339744831;
	float radius;
	float angle;
	if ( abs( p.x ) > abs( p.y ) ) {
		radius = p.x;
		angle = quarter * ( p.y / p.x );
	} else {
		radius = p.y;
		angle = 2.0 * quarter - quarter * ( p.x / p.y );
	}
	return radius * float2( cos( angle ), sin( angle ) );
}
