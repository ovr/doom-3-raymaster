// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

// Procedural noise for the effect programs: integer hashes, so every GPU gives the same bits.

uint HashUint( uint x ) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// 0..1 from a lattice point and a seed
float Hash21( int2 p, uint seed ) {
	const uint h = HashUint( uint( p.x ) * 0x9e3779b9u + HashUint( uint( p.y ) + seed * 0x85ebca6bu ) );
	return float( h >> 8 ) * ( 1.0 / 16777216.0 );
}

// value noise, 0..1, smooth across lattice cells
float ValueNoise( float2 p, uint seed ) {
	const float2 cell = floor( p );
	const float2 f = p - cell;
	const float2 u = f * f * ( 3.0 - 2.0 * f );
	const int2 i = int2( cell );
	const float a = Hash21( i, seed );
	const float b = Hash21( i + int2( 1, 0 ), seed );
	const float c = Hash21( i + int2( 0, 1 ), seed );
	const float d = Hash21( i + int2( 1, 1 ), seed );
	return lerp( lerp( a, b, u.x ), lerp( c, d, u.x ), u.y );
}

// three octaves, 0..1
float Fbm( float2 p, uint seed ) {
	float sum = 0.5 * ValueNoise( p, seed );
	sum += 0.25 * ValueNoise( p * 2.03 + 17.0, seed + 1 );
	sum += 0.125 * ValueNoise( p * 4.01 + 43.0, seed + 2 );
	return sum * ( 1.0 / 0.875 );
}

float Hash31( int3 p, uint seed ) {
	const uint h = HashUint( uint( p.x ) * 0x9e3779b9u + HashUint( uint( p.y ) * 0xc2b2ae35u + HashUint( uint( p.z ) + seed * 0x85ebca6bu ) ) );
	return float( h >> 8 ) * ( 1.0 / 16777216.0 );
}

// value noise in a volume, 0..1
float ValueNoise3( float3 p, uint seed ) {
	const float3 cell = floor( p );
	const float3 f = p - cell;
	const float3 u = f * f * ( 3.0 - 2.0 * f );
	const int3 i = int3( cell );
	const float x00 = lerp( Hash31( i, seed ), Hash31( i + int3( 1, 0, 0 ), seed ), u.x );
	const float x10 = lerp( Hash31( i + int3( 0, 1, 0 ), seed ), Hash31( i + int3( 1, 1, 0 ), seed ), u.x );
	const float x01 = lerp( Hash31( i + int3( 0, 0, 1 ), seed ), Hash31( i + int3( 1, 0, 1 ), seed ), u.x );
	const float x11 = lerp( Hash31( i + int3( 0, 1, 1 ), seed ), Hash31( i + int3( 1, 1, 1 ), seed ), u.x );
	return lerp( lerp( x00, x10, u.y ), lerp( x01, x11, u.y ), u.z );
}

// three octaves in a volume, 0..1
float Fbm3( float3 p, uint seed ) {
	float sum = 0.5 * ValueNoise3( p, seed );
	sum += 0.25 * ValueNoise3( p * 2.03 + 17.0, seed + 1 );
	sum += 0.125 * ValueNoise3( p * 4.01 + 43.0, seed + 2 );
	return sum * ( 1.0 / 0.875 );
}
