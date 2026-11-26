// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	A hit of the GPU scene lit as the linear phase lights a pixel (R13): interaction.hlsl and interactionAmbient.hlsl
	per light, the same decodes and modifiers, then the emissive stage added in linear. The lights are GpuScene.h's
	gpuSceneLight_t, 144 bytes, all of the scene's areas:

	  0    origin           xyz globalLightOrigin
	  16   project[4]       world planes: S, T and Q of the projection (texture matrix baked in), the falloff's S
	  80   color            r_lightScale times the stage's colour, gamma
	  96   boundsMin, Max   xyz
	  128  projection, falloff, flags: views of 2D textures, gpuSceneLight_t::LIGHT_*
	  140  emitterRadius    of the disk at origin that emits, 0 a point

	A light reaches a point inside its bounds, its projection's [0, 1] square in front of it and its falloff's [0, 1]
	(the raster draws only what lies in the light's volume). Of those, the first maxLights that add light are taken,
	each with a shadow ray when it casts shadows. Vertex colours are taken as white.
*/

#ifndef RTLIGHTING_HLSLI
#define RTLIGHTING_HLSLI

#include "raytracing.hlsli"
#include "srgb.hlsli"

static const uint RT_LIGHT_BYTES = 144;
static const uint RT_LIGHT_SHADOWS = 1u << 0;
static const uint RT_LIGHT_AMBIENT = 1u << 1;

struct RtLight {
	float3	origin;
	float4	project[4];
	float3	color;
	float3	boundsMin;
	float3	boundsMax;
	uint	projection;
	uint	falloff;
	uint	flags;
	float	radius;
};

// What lights a hit.
struct RtLighting {
	uint	lights;			// the light buffer's raw view
	uint	numLights;
	uint	maxLights;
	bool	shadows;
	uint	linearSampler;	// repeating: the surface's textures
	uint	clampSampler;	// the lights' textures
};

RtLight RtLoadLight( uint lights, uint index ) {
	ByteAddressBuffer buffer = ResourceDescriptorHeap[lights];
	const uint at = index * RT_LIGHT_BYTES;
	RtLight light;
	light.origin = asfloat( buffer.Load3( at ) );
	[unroll]
	for ( uint i = 0; i < 4; i++ ) {
		light.project[i] = asfloat( buffer.Load4( at + 16 + i * 16 ) );
	}
	light.color = asfloat( buffer.Load3( at + 80 ) );
	light.boundsMin = asfloat( buffer.Load3( at + 96 ) );
	light.boundsMax = asfloat( buffer.Load3( at + 112 ) );
	const uint3 views = buffer.Load3( at + 128 );
	light.projection = views.x;
	light.falloff = views.y;
	light.flags = views.z;
	light.radius = asfloat( buffer.Load( at + 140 ) );
	return light;
}

// The radiance of a hit toward the ray's origin, unexposed. The pixel's cone of pixelSpread radians has come
// coneDistance before the ray's origin: its width at the hit is the textures' level.
float3 RtShade( RtHeap heap, RtLighting lighting, RtHit hit, RayDesc ray, float coneDistance, float pixelSpread ) {
	const RtGeometry g = RtLoadGeometry( heap, hit.record );
	if ( ( g.flags & RT_GEO_NO_SHADING ) != 0 ) {
		return float3( 0.0, 0.0, 0.0 );
	}
	const RtSurface s = RtSurfaceOf( heap, g, hit.instanceIndex, hit.primitive, hit.barycentrics, hit.objectToWorld, hit.worldToObject );
	const float footprint = RtFootprint( coneDistance + hit.t, pixelSpread, ray.Direction, s.faceNormal );
	const float3 rayOrigin = ray.Origin;

	// the decodes of interaction.hlsl
	const float4 bump = RtSample( g, RT_TEXTURE_BUMP, s, footprint, lighting.linearSampler );
	float3 localNormal;
	localNormal.xy = bump.wy - 0.5;
	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );
	localNormal = normalize( localNormal );
	const float3 diffuse = SRGBToLinear( ConvertYCoCgToRGB( RtSample( g, RT_TEXTURE_DIFFUSE, s, footprint, lighting.linearSampler ) ) );
	const float3 specMap = SRGBToLinear( RtSample( g, RT_TEXTURE_SPECULAR, s, footprint, lighting.linearSampler ).rgb );
	const float3 toView = normalize( mul( hit.worldToObject, float4( rayOrigin, 1.0 ) ) - s.objectPosition );
	SamplerState clampSampler = SamplerDescriptorHeap[lighting.clampSampler];

	float3 radiance = float3( 0.0, 0.0, 0.0 );
	uint taken = 0;
	for ( uint i = 0; i < lighting.numLights && taken < lighting.maxLights; i++ ) {
		const RtLight light = RtLoadLight( lighting.lights, i );
		if ( any( s.position < light.boundsMin ) || any( s.position > light.boundsMax ) ) {
			continue;
		}
		const float4 p = float4( s.position, 1.0 );
		const float q = dot( p, light.project[2] );
		const float f = dot( p, light.project[3] );
		if ( q <= 0.0 || f < 0.0 || f > 1.0 ) {
			continue;
		}
		const float2 st = float2( dot( p, light.project[0] ), dot( p, light.project[1] ) ) / q;
		if ( any( st < 0.0 ) || any( st > 1.0 ) ) {
			continue;
		}
		Texture2D<float4> projectionMap = ResourceDescriptorHeap[NonUniformResourceIndex( light.projection )];
		Texture2D<float4> falloffMap = ResourceDescriptorHeap[NonUniformResourceIndex( light.falloff )];
		const float3 filter = SRGBToLinear( projectionMap.SampleLevel( clampSampler, st, 0.0 ).rgb )
			* SRGBToLinear( falloffMap.SampleLevel( clampSampler, float2( f, 0.5 ), 0.0 ).rgb );

		// interaction.hlsl in the hit's object space, per pixel what its vertex program does per vertex
		const float3 toLight = mul( hit.worldToObject, float4( light.origin, 1.0 ) ) - s.objectPosition;
		const float3 tangentToLight = float3( dot( s.objectTangent, toLight ), dot( s.objectBinormal, toLight ), dot( s.objectNormal, toLight ) );
		const bool ambient = ( light.flags & RT_LIGHT_AMBIENT ) != 0;
		const float3 lightVector = normalize( ambient ? float3( 0.5, 9.5 - 0.385, 0.8925 ) : tangentToLight );
		const float3 halfAngle = normalize( toLight ) + toView;
		const float3 tangentHalfAngle = normalize( float3( dot( s.objectTangent, halfAngle ), dot( s.objectBinormal, halfAngle ),
			dot( s.objectNormal, halfAngle ) ) );
		const float specular = pow( abs( dot( tangentHalfAngle, localNormal ) ), 10.0 );	// an even power, as GL's pow
		// RB_ColorParm decodes the modifier as one colour: the light's times the stage's
		const float3 diffuseModifier = SRGBToLinear( light.color * g.colors[0].rgb );
		// an ambient light draws with a black specular map (RB_DrawSingleInteraction)
		const float3 specularModifier = ambient ? float3( 0.0, 0.0, 0.0 ) : SRGBToLinear( 2.0 * light.color * g.colors[1].rgb );
		const float3 lit = max( ( diffuse * diffuseModifier + specMap * specular * specularModifier ) * dot( lightVector, localNormal ) * filter, 0.0 );
		if ( all( lit <= 0.0 ) ) {
			continue;
		}
		taken++;
		if ( lighting.shadows && ( light.flags & RT_LIGHT_SHADOWS ) != 0 ) {
			// Off the plane on the side the ray came to, by more than the hit's float error. A triangle that faces away
			// from the light shadows itself, as the cap of its stencil volume: the ray comes back through the hit.
			const float3 side = hit.frontFace ? s.faceNormal : -s.faceNormal;
			const float3 aim = normalize( light.origin - s.position );
			const float lift = 0.02 + 0.1 * footprint;
			const float rise = dot( aim, side );
			const float3 origin = ( rise > 0.0 ) ? s.position + side * lift : s.position - aim * ( lift / max( -rise, 0.05 ) );
			const float3 path = light.origin - origin;
			const float distance = length( path );
			if ( distance > 0.0 && RtOccluded( heap, origin, path / distance, distance, footprint, lighting.linearSampler, RtShadowMask( g.flags ) ) ) {
				continue;
			}
		}
		radiance += lit;
	}

	// an unlit stage adds in gamma over the lights in the raster; here its linear value
	if ( any( g.colors[2].rgb > 0.0 ) ) {
		radiance += SRGBToLinear( RtSample( g, RT_TEXTURE_EMISSIVE, s, footprint, lighting.linearSampler ).rgb * g.colors[2].rgb );
	}
	return radiance;
}

#endif
