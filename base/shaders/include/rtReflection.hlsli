// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	What the passes of the reflections share (R20 of RenderBackend.h). internal/rtReflect.hlsl leaves, a texel per
	pixel, the radiance its ray met, unexposed and before the surface's Fresnel, and in alpha how far the ray went
	(the longest ray on a miss), or one of

	  RT_REFLECT_NONE    nothing reflects here: no surface, F0 0, too rough
	  RT_REFLECT_MIRROR  a mirror's image: one ray about the normal, the same every frame, final as it is

	A glossy pixel's ray leaves about a normal of the GGX distribution of roughness squared, drawn as the eye sees
	the facets (without the lobe's tail, RT_REFLECT_LOBE), and counts whole: the mean over the rays is the lobe's
	average of what the surface faces, and a surface under an even sky reflects that sky at any roughness. The
	filters keep, beside the mean,

	  spread  RGBA16F  r the variance of the mean's luminance, g how far the rays went on average, b the roughness,
	                   a RT_REFLECT_KIND_*: nothing, a value that is final, a mean to filter

	The surface's part, Fresnel and the fade toward the roughest that reflects, is internal/rtComposite.hlsl's: the
	filters work on what the specular map does not modulate.
*/

static const float RT_REFLECT_NONE = -1.0;
static const float RT_REFLECT_MIRROR = -2.0;
static const float RT_REFLECT_FAR = 60000.0;	// the farthest a half float tells

static const float RT_REFLECT_KIND_NONE = 0.0;
static const float RT_REFLECT_KIND_FINAL = 1.0;
static const float RT_REFLECT_KIND_MEAN = 2.0;

// 0 where a pixel is too rough to reflect. limits: x the roughness that reflects nothing, y the fade below it.
float RtReflectWeight( float roughness, float2 limits ) {
	return saturate( ( limits.x - roughness ) / limits.y );
}

// Schlick's Fresnel, capped by the smoothness: a rough surface turns to no mirror at a grazing angle.
float3 RtReflectFresnel( float3 f0, float roughness, float facing ) {
	return f0 + ( max( 1.0 - roughness, f0 ) - f0 ) * pow( 1.0 - saturate( facing ), 5.0 );
}

// The part of the lobe a ray is drawn from. The facets turned farthest are its long tail: a ray of the last tenth
// lands lobes away from the others, a lone dot that no filter of the lobe's width reaches.
static const float RT_REFLECT_LOBE = 0.9;

// A normal of the GGX facets that toEye sees, in the frame of the surface's normal (z, toEye.z above 0): a point of
// the sphere's cap above the stretched view's horizon, plus that view, unstretched. u: two numbers in [0, 1).
float3 RtGgxVisibleNormal( float3 toEye, float alpha, float2 u ) {
	const float3 stretched = normalize( float3( toEye.xy * alpha, toEye.z ) );
	const float angle = 6.28318530717958648 * u.x;
	const float z = ( 1.0 - u.y * RT_REFLECT_LOBE ) * ( 1.0 + stretched.z ) - stretched.z;
	const float sine = sqrt( saturate( 1.0 - z * z ) );
	const float3 facet = float3( sine * cos( angle ), sine * sin( angle ), z ) + stretched;
	return normalize( float3( facet.xy * alpha, max( facet.z, 1e-6 ) ) );
}

// How far, in pixels, the lobe of a pixel reaches over its neighbours: two pixels this far apart see, about their
// normals, points of the reflected scene one lobe's width apart. depth: the pixel's distance; reach: its rays'.
float RtReflectLobePixels( float roughness, float reach, float depth, float pixelSpread ) {
	return roughness * roughness * reach / max( ( depth + reach ) * pixelSpread, 1e-6 );
}

// 1 for a neighbour turned as the pixel is, less as their lobes part: the reflected ray turns twice as far as the
// normal. The guides' normals are 7 bits a side: a step of theirs is no other surface.
float RtReflectFacing( float3 normal, float3 other, float roughness ) {
	const float alpha = roughness * roughness;
	return exp( -( 1.0 - dot( normal, other ) ) / ( alpha * alpha + 8e-4 ) );
}
