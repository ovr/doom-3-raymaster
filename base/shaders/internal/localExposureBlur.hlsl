// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The blurs of the local exposure, both on ( sum, count ) pairs, so the mean of a blurred cell is the weighted
	mean of the log luminance under it and cells outside the grid simply do not count:

	  z < GRID_BINS    the bilateral grid's cell x, y, z by a binomial kernel, 5 x 5 tiles and 3 bins
	  z = GRID_BINS    the blurred luminance of tile x, y: the tiles' totals by a Gaussian of sigma tiles
*/

#define GRID_BINS 32

cbuffer BlurParms : register( b0 ) {
	uint gridSrv;
	uint totalsSrv;
	uint gridUav;			// the blurred grid
	uint blurredUav;
	uint tilesX;
	uint tilesY;
	float invTwoSigmaSquared;
	int radius;				// tiles on each side
};

[numthreads( 4, 4, 4 )]
void CSMain( uint3 id : SV_DispatchThreadID ) {
	if ( id.x >= tilesX || id.y >= tilesY || id.z > GRID_BINS ) {
		return;
	}
	const int3 size = int3( tilesX, tilesY, GRID_BINS );
	float2 sum = 0.0;
	if ( id.z < GRID_BINS ) {
		static const float BINOMIAL5[5] = { 1.0, 4.0, 6.0, 4.0, 1.0 };
		static const float BINOMIAL3[3] = { 1.0, 2.0, 1.0 };
		Texture3D<float2> grid = ResourceDescriptorHeap[gridSrv];
		for ( int dz = -1; dz <= 1; dz++ ) {
			for ( int dy = -2; dy <= 2; dy++ ) {
				for ( int dx = -2; dx <= 2; dx++ ) {
					const int3 p = int3( id ) + int3( dx, dy, dz );
					if ( all( p >= 0 ) && all( p < size ) ) {
						sum += grid.Load( int4( p, 0 ) ) * ( BINOMIAL5[dx + 2] * BINOMIAL5[dy + 2] * BINOMIAL3[dz + 1] );
					}
				}
			}
		}
		RWTexture3D<float2> blurredGrid = ResourceDescriptorHeap[gridUav];
		blurredGrid[id] = sum * ( 1.0 / 1024.0 );
		return;
	}
	Texture2D<float2> totals = ResourceDescriptorHeap[totalsSrv];
	for ( int y = max( int( id.y ) - radius, 0 ); y <= min( int( id.y ) + radius, size.y - 1 ); y++ ) {
		for ( int x = max( int( id.x ) - radius, 0 ); x <= min( int( id.x ) + radius, size.x - 1 ); x++ ) {
			const float2 d = float2( x - int( id.x ), y - int( id.y ) );
			sum += totals.Load( int3( x, y, 0 ) ) * exp( -dot( d, d ) * invTwoSigmaSquared );
		}
	}
	RWTexture2D<float2> blurred = ResourceDescriptorHeap[blurredUav];
	blurred[id.xy] = sum;
}
