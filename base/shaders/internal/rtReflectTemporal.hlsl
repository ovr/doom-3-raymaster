// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	The glossy reflections over the frames (R20, R18): the frame's ray (internal/rtReflect.hlsl) joins the mean its
	pixel had the frame before. That mean is looked for at two places, each taken only from the pixel's surface
	(include/rtDenoise.hlsli): where the surface was, by the G-buffer's motion, and where the reflected image was,
	the point as far behind the surface along the eye's line as the rays went, through the frame before's world to
	clip: a mirror's image moves as that point does, not as the mirror. Of the two the one nearer to what the
	pixel's neighbours show now is taken.

	  signal         in   the frame's rays (include/rtReflection.hlsli)
	  colour, aux    out  rgb the mean, a the frames in it (at most maxHistory); r the mean of the luminance
	                      squared, g of how far the rays went: the history
	  output         out  include/rtReflection.hlsli's spread: with the colour, what the spatial filter works on

	A mean of RT_CUT_FRAMES frames or more that lies outside the range of the pixel's 5 x 5 neighbours of its
	surface in this frame, by more than its own noise and theirs explain, is cut to that range and counts for
	fewer frames by as much: what the surface reflects has changed. Fewer frames tell no variance: the neighbours
	do then. A mirror's image and a pixel that reflects nothing pass as they are and keep no history; a hole the
	guides bridged keeps what it had. flags bit 2, a frame without rays: a glossy pixel shows what its history
	holds, uncounted, and nothing where it holds nothing. MULTISAMPLED reads sample 0 of the scene.
*/

#include "../include/rtDenoise.hlsli"
#include "../include/rtView.hlsli"
#include "../include/rtReflection.hlsli"

cbuffer ReflectTemporalParms : register( b0 ) {
	float4 clipToWorld[4];		// rows: GL's clip space to world
	float4 prevWorldToClip[4];	// rows, of the frame before
	float4 origin;				// xyz the eye
	float4 viewport;			// x, y, width, height in the scene
	float4 limits;				// x the roughness that reflects nothing, y the fade below it, z below this roughness a mirror
	int2 first;					// the first pixel in the scene
	uint2 size;					// texels
	uint signalSrv;
	uint guidesSrv;
	uint reprojectSrv;
	uint slopeSrv;
	uint prevGuidesSrv;
	uint prevColorSrv;
	uint prevAuxSrv;
	uint depthSrv;
	uint materialSrv;			// GB1
	uint colorUav;
	uint auxUav;
	uint outputUav;
	uint flags;					// bit 0: the three of the frame before are there; 1: a plain average, nothing cut; 2: no ray this frame
	float maxHistory;
	float leastStep;			// RtAverage's: the last bit of the history's format over its value, or 0
	uint unused;
};

static const float CUT_DEVIATIONS = 2.0;	// of the difference between the mean and the neighbours' that noise explains
static const float CUT_PART = 0.25;			// a cut by this part of the neighbours' mean starts the history anew

struct Held {
	float3	mean;
	float	frames;		// 0: nothing of the pixel's surface is there
	float	noise;		// the variance of one frame's luminance
	float	reach;		// how far the rays went on average
};

Held NoHistory() {
	Held held;
	held.mean = float3( 0.0, 0.0, 0.0 );
	held.frames = 0.0;
	held.noise = 0.0;
	held.reach = 0.0;
	return held;
}

// The four texels of the frame before around at, each only of the pixel's surface, as differences to the first one:
// equal ones give their value to the bit. Their variances are mixed, not their second moments.
Held ReadHeld( float2 at, RtGuide guide, float expected, float tolerance ) {
	Texture2D<uint> prevGuides = ResourceDescriptorHeap[prevGuidesSrv];
	Texture2D<float4> prevColor = ResourceDescriptorHeap[prevColorSrv];
	Texture2D<float2> prevAux = ResourceDescriptorHeap[prevAuxSrv];
	Held held = NoHistory();
	const float2 base = floor( at );
	const float2 part = at - base;
	float4 one = float4( 0.0, 0.0, 0.0, 0.0 );
	float2 oneRest = float2( 0.0, 0.0 );	// noise, reach
	float4 sum = float4( 0.0, 0.0, 0.0, 0.0 );
	float2 sumRest = float2( 0.0, 0.0 );
	float weight = 0.0;
	[unroll]
	for ( uint tap = 0; tap < 4; tap++ ) {
		const int2 offset = int2( tap & 1u, tap >> 1 );
		const float w = ( ( offset.x != 0 ) ? part.x : 1.0 - part.x ) * ( ( offset.y != 0 ) ? part.y : 1.0 - part.y );
		const int2 q = int2( base ) + offset;
		if ( w > 0.0 && all( q >= 0 ) && all( q < int2( size ) ) ) {
			const float4 kept = prevColor.Load( int3( q, 0 ) );
			if ( kept.a >= 1.0 && RtSameSurface( guide, expected, tolerance, RtUnpackGuide( prevGuides.Load( int3( q, 0 ) ) ) ) ) {
				const float2 aux = prevAux.Load( int3( q, 0 ) );
				const float luminance = dot( kept.rgb, RT_LUMA );
				const float2 rest = float2( aux.x - luminance * luminance, aux.y );
				if ( weight == 0.0 ) {
					one = kept;
					oneRest = rest;
				}
				sum += w * ( kept - one );
				sumRest += w * ( rest - oneRest );
				weight += w;
			}
		}
	}
	if ( weight > 0.0 ) {
		const float4 mixed = one + sum / weight;
		const float2 rest = oneRest + sumRest / weight;
		held.mean = mixed.rgb;
		held.frames = mixed.a;
		held.noise = rest.x;
		held.reach = rest.y;
	}
	return held;
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 dispatch : SV_DispatchThreadID ) {
	if ( any( dispatch.xy >= size ) ) {
		return;
	}
	Texture2D<uint> guides = ResourceDescriptorHeap[guidesSrv];
	RWTexture2D<float4> colorOut = ResourceDescriptorHeap[colorUav];
	RWTexture2D<float2> auxOut = ResourceDescriptorHeap[auxUav];
	RWTexture2D<float4> output = ResourceDescriptorHeap[outputUav];

	const int2 texel = int2( dispatch.xy );
	const int2 pixel = first + texel;
	const uint packed = guides.Load( int3( texel, 0 ) );
	const bool past = ( flags & 1u ) != 0;
	const bool plain = ( flags & 2u ) != 0;
	const bool hold = ( flags & 4u ) != 0;
	const RtGuide guide = RtUnpackGuide( packed );
	if ( !guide.drawn || guide.bridged ) {
		// a hole of the G-buffer reflects nothing this frame and keeps what it had for the next
		float4 kept = float4( 0.0, 0.0, 0.0, 0.0 );
		float2 keptAux = float2( 0.0, 0.0 );
		if ( guide.drawn && past ) {
			Texture2D<float4> prevColor = ResourceDescriptorHeap[prevColorSrv];
			Texture2D<float2> prevAux = ResourceDescriptorHeap[prevAuxSrv];
			kept = prevColor.Load( int3( texel, 0 ) );
			keptAux = prevAux.Load( int3( texel, 0 ) );
		}
		colorOut[dispatch.xy] = kept;
		auxOut[dispatch.xy] = keptAux;
		output[dispatch.xy] = float4( 0.0, 0.0, 0.0, RT_REFLECT_KIND_NONE );
		return;
	}

	RT_SCENE_TEXTURE( float4 ) materialMap = ResourceDescriptorHeap[materialSrv];
	const float4 material = RT_SCENE_LOAD( materialMap, pixel );
	const float roughness = material.a;
	RtView eye;
	eye.clipToWorld = clipToWorld;
	eye.eye = origin.xyz;
	eye.viewport = viewport;
	eye.depthSrv = depthSrv;
	const float depth = RtDepthAt( eye, pixel );

	Texture2D<float4> signal = ResourceDescriptorHeap[signalSrv];
	float4 now = float4( 0.0, 0.0, 0.0, RT_REFLECT_NONE );
	if ( !hold ) {
		now = signal.Load( int3( texel, 0 ) );
	} else if ( depth < 1.0 && roughness >= limits.z && RtReflectWeight( roughness, limits.xy ) > 0.0 && any( material.rgb > 0.0 ) ) {
		// what would have sent a glossy ray
		now.a = 0.0;
	}
	if ( now.a < 0.0 ) {
		const bool mirror = now.a < 0.5 * ( RT_REFLECT_NONE + RT_REFLECT_MIRROR );
		colorOut[dispatch.xy] = float4( mirror ? now.rgb : float3( 0.0, 0.0, 0.0 ), 0.0 );
		auxOut[dispatch.xy] = float2( 0.0, 0.0 );
		output[dispatch.xy] = float4( 0.0, 0.0, roughness, mirror ? RT_REFLECT_KIND_FINAL : RT_REFLECT_KIND_NONE );
		return;
	}
	Texture2D<float2> slopes = ResourceDescriptorHeap[slopeSrv];
	const float2 slope = slopes.Load( int3( texel, 0 ) );

	// the pixel's neighbours of this frame that are its surface: their range, their mean and their spread
	const float nowLuminance = dot( now.rgb, RT_LUMA );
	float3 least = now.rgb;
	float3 most = now.rgb;
	float sum = nowLuminance;
	float squares = nowLuminance * nowLuminance;
	float taken = 1.0;
	if ( !hold ) {
		for ( int y = -2; y <= 2; y++ ) {
			for ( int x = -2; x <= 2; x++ ) {
				const int2 q = texel + int2( x, y );
				if ( ( x != 0 || y != 0 ) && all( q >= 0 ) && all( q < int2( size ) ) ) {
					const float4 other = signal.Load( int3( q, 0 ) );
					const RtGuide otherGuide = RtUnpackGuide( guides.Load( int3( q, 0 ) ) );
					if ( other.a >= 0.0 && otherGuide.drawn && RtPlaneWeight( guide.w, slope, float2( x, y ), otherGuide.w ) > 0.5
							&& RtReflectFacing( guide.normal, otherGuide.normal, roughness ) > 0.5 ) {
						const float l = dot( other.rgb, RT_LUMA );
						least = min( least, other.rgb );
						most = max( most, other.rgb );
						sum += l;
						squares += l * l;
						taken += 1.0;
					}
				}
			}
		}
	}
	const float mid = sum / taken;
	const float around = max( squares / taken - mid * mid, 0.0 );

	Held held = NoHistory();
	if ( past ) {
		Texture2D<float4> reproject = ResourceDescriptorHeap[reprojectSrv];
		const float3 moved = reproject.Load( int3( texel, 0 ) ).xyz;
		// the scene counts its rows up, as the motion does: where the pixel's centre was, in texels
		const float2 atSurface = float2( texel ) - moved.xy;
		const float expected = guide.w * moved.z;
		const float tolerance = 0.02 + abs( slope.x ) + abs( slope.y );
		held = ReadHeld( atSurface, guide, expected, tolerance );
		if ( !hold ) {
			const float3 position = RtPositionAt( eye, pixel, depth );
			const float reach = ( held.frames >= 1.0 ) ? lerp( held.reach, now.a, 1.0 / ( held.frames + 1.0 ) ) : now.a;
			const float4 image = float4( position + normalize( position - origin.xyz ) * reach, 1.0 );
			const float w = dot( prevWorldToClip[3], image );
			if ( w > 1e-6 ) {
				const float2 ndc = float2( dot( prevWorldToClip[0], image ), dot( prevWorldToClip[1], image ) ) / w;
				const float2 atImage = viewport.xy + ( ndc * 0.5 + 0.5 ) * viewport.zw - 0.5 - float2( first );
				// the surface's point that showed the image then lies apart on the pixel's plane
				const float2 apart = atImage - atSurface;
				const float along = 1.0 + dot( slope, apart );
				if ( dot( apart, apart ) > 4e-4 && along > 0.25 ) {
					const Held other = ReadHeld( atImage, guide, expected / along,
						tolerance + 0.25 * ( abs( slope.x * apart.x ) + abs( slope.y * apart.y ) ) );
					if ( other.frames >= 1.0 && ( held.frames < 1.0
							|| abs( dot( other.mean, RT_LUMA ) - mid ) < abs( dot( held.mean, RT_LUMA ) - mid ) ) ) {
						held = other;
					}
				}
			}
		}
	}

	if ( hold && held.frames < 1.0 ) {
		colorOut[dispatch.xy] = float4( 0.0, 0.0, 0.0, 0.0 );
		auxOut[dispatch.xy] = float2( 0.0, 0.0 );
		output[dispatch.xy] = float4( 0.0, 0.0, roughness, RT_REFLECT_KIND_NONE );
		return;
	}

	if ( !hold && !plain && held.frames >= RT_CUT_FRAMES ) {
		const float explained = CUT_DEVIATIONS * sqrt( max( held.noise, around ) * ( 1.0 / held.frames + 1.0 / taken ) );
		const float3 cut = clamp( held.mean, least - explained, most + explained );
		const float3 off = abs( cut - held.mean );
		const float part = saturate( max( off.r, max( off.g, off.b ) ) / ( CUT_PART * max( mid, 1e-4 ) ) );
		held.mean = cut;
		held.frames = lerp( held.frames, min( held.frames, RT_CUT_FRAMES - 1.0 ), part );
		held.noise = lerp( held.noise, around, part );
	}

	// a frame without rays counts for none: what is held stands
	const float count = hold ? held.frames : min( held.frames + 1.0, maxHistory );
	const float heldLuminance = dot( held.mean, RT_LUMA );
	float3 mean = held.mean;
	float second = heldLuminance * heldLuminance + held.noise;
	float reach = held.reach;
	if ( !hold ) {
		const float blend = 1.0 / count;
		mean = RtAverage( mean, now.rgb, blend, leastStep );
		second = RtAverage( second, nowLuminance * nowLuminance, blend, leastStep );
		reach = ( held.frames >= 1.0 ) ? RtAverage( reach, now.a, blend, leastStep ) : now.a;
	}
	// what the next frame reads; the reference keeps its mean in 32 bits
	if ( leastStep > 0.0 ) {
		mean = RtHalf( mean );
		second = RtHalf( second );
		reach = RtHalf( reach );
	}
	const float known = RtKnownNoise( dot( mean, RT_LUMA ), second );
	const float variance = ( count < RT_CUT_FRAMES ) ? max( known, around ) : known;
	colorOut[dispatch.xy] = float4( mean, count );
	auxOut[dispatch.xy] = float2( second, reach );
	// of the mean, not of one frame
	output[dispatch.xy] = float4( variance / count, reach, roughness, RT_REFLECT_KIND_MEAN );
}
