// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The indirect light as its passes keep it (R19), unexposed, a texel per stride pixels a side:

	  amount  RGBA16F  a the mean luminance (Y of YCoCg) of what the rays brought, rgb the mean of that times each
	                   ray's direction
	  aux     RGBA16F  r, g the mean Co and Cg, b the mean of the luminance squared (a filter's pass: the variance of
	                   the mean), a the frames in the means; 0: the texel holds nothing

	The rays leave by the cosine about the plane the depth holds, so the mean luminance times the albedo is what a
	surface along the plane sends on, and the bumps of a wall share one signal. A pixel takes it by its own normal
	(RtBounceShare), as if the light were an even glow of the half sphere plus light from one direction: the two
	parts that give the kept amount and mean direction. Exact for either and for any mix of them.
*/

static const float RT_BOUNCE_MOST = 250.0;		// the square of a luminance has to fit a half float

// r the luminance, gb Co and Cg.
float3 RtBounceEncode( float3 rgb ) {
	return float3( dot( rgb, float3( 0.25, 0.5, 0.25 ) ), 0.5 * ( rgb.r - rgb.b ), dot( rgb, float3( -0.25, 0.5, -0.25 ) ) );
}

float3 RtBounceDecode( float luminance, float2 chroma ) {
	return float3( luminance + chroma.x - chroma.y, luminance + chroma.y, luminance - chroma.x - chroma.y );
}

// What a surface turned along normal takes of the light kept for one along plane, as a part of it. An even glow has
// the mean direction 2 / 3 plane; with a part m of the amount from the direction s the rest u of it is
// m s - 2 / 3 m plane, and |s| = 1 gives m. In terms of normal - plane, so that a flat surface takes exactly 1.
float RtBounceShare( float3 normal, float3 plane, float4 amount ) {
	if ( amount.a <= 0.0 ) {
		return 1.0;
	}
	const float3 u = amount.xyz / amount.a - ( 2.0 / 3.0 ) * plane;
	const float c = dot( u, plane );
	const float part = saturate( 0.6 * ( 2.0 * c + sqrt( 4.0 * c * c + 5.0 * dot( u, u ) ) ) );
	const float3 from = u + ( 2.0 / 3.0 ) * part * plane;
	const float3 lean = normal - plane;
	const float wide = 1.0 + 0.5 * dot( lean, plane );
	const float along = dot( plane, from );
	const float narrow = ( along > 1e-3 * length( from ) ) ? clamp( 1.0 + dot( lean, from ) / along, 0.0, 3.0 ) : 1.0;
	return lerp( wide, narrow, part );
}
