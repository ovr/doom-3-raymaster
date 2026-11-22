// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

// The light of the game's effect system (idFxSystem): glows, sparks, flashes and hot marks, drawn from the quad's
// coordinates alone and added to the scene as HDR radiance. The vertex carries the particle in the bytes an unlit
// surface leaves free: NORMAL = age, seed, heat, kind; TANGENT = intensity, depth fade, two spares.

#include "include/common.hlsli"
#include "include/noise.hlsli"

cbuffer VertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
};

cbuffer PixelParms : register( b1 ) {
	float4 rpProjectionMatrixZ;
	float4 rpSoftParticle;		// zw: the viewport's origin (texel 0 of the depth copy)
};

TEXTURE2D( Texture2D, viewDepthMap, 0 );	// the view's depth copy: GL window z

static const uint KIND_GLOW = 0;		// a core with a halo
static const uint KIND_SPARK = 1;		// a streak, u from the tail to the head
static const uint KIND_MARK = 2;		// a hot spot on a surface, cooling
static const uint KIND_FLASH = 3;		// a burst with rays
static const uint KIND_ORB = 4;			// an unstable ball of plasma

// idRenderMatrix::ApplyDepthHack: the weapon's clip z is a quarter of the true one
static const float WEAPON_DEPTH_SCALE = 0.25;

static const int ORB_STEPS = 10;		// samples along the ray through an orb
static const int ARC_OCTAVES = 5;

struct VSInput {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
	float4 normal : NORMAL;
	float4 tangent : TANGENT;
	float4 color : COLOR0;
};

struct VSOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
	float4 particle : TEXCOORD1;	// age, seed, heat, kind
	float4 light : TEXCOORD2;		// the tint in linear times the intensity, the envelope
	float fadeScale : TEXCOORD3;	// 1 / the depth over which it fades at geometry, 0 = no fade
	float time : TEXCOORD4;			// seconds, wraps at 256
};

VSOutput VSMain( VSInput input ) {
	VSOutput output;
	float4 glPosition;
	glPosition.x = dot4( input.position, rpMVPmatrixX );
	glPosition.y = dot4( input.position, rpMVPmatrixY );
	glPosition.z = dot4( input.position, rpMVPmatrixZ );
	glPosition.w = dot4( input.position, rpMVPmatrixW );
	precise float4 position = GLClipToD3D( glPosition );
	output.position = position;
	output.texcoord = input.texcoord;
	output.particle = float4( input.normal.xy, input.normal.z, round( input.normal.w * 255.0 ) );
	// the intensity byte is an exponent: 2^( b / 16 - 4 ), 1/16 .. 3900
	const float intensity = exp2( input.tangent.x * ( 255.0 / 16.0 ) - 4.0 );
	output.light = float4( SRGBToLinear( input.color.rgb ) * intensity, input.color.a );
	const float fadeDepth = input.tangent.y * 64.0;
	output.fadeScale = ( fadeDepth > 0.0 ) ? 1.0 / fadeDepth : 0.0;
	output.time = round( input.tangent.z * 255.0 ) + round( input.tangent.w * 255.0 ) * ( 1.0 / 256.0 );
	return output;
}

// as texture_color.hlsl: the far clear lies past the plane of GL's infinite projection, which reads as no depth
float ViewDepth( float windowZ ) {
	const float denominator = -rpProjectionMatrixZ.z - ( windowZ * 2.0 - 1.0 );
	return ( denominator > 0.0 ) ? -rpProjectionMatrixZ.w / denominator : 1e30;
}

// the colour of a glowing body by its heat 0..1: dark red, orange, yellow, white; it dims as it cools
float3 HeatColor( float heat ) {
	const float3 color = saturate( float3( heat * 3.0, heat * 1.6 - 0.35, heat * 3.0 - 2.2 ) );
	return color * heat * heat;
}

float4 PSMain( VSOutput input ) : SV_Target {
	const float age = input.particle.x;
	const uint seed = uint( input.particle.y * 255.0 + 0.5 );
	const float heat = input.particle.z;
	const uint kind = uint( input.particle.w + 0.5 );
	const float2 p = input.texcoord * 2.0 - 1.0;
	const float r = length( p );

	float3 radiance;
	if ( kind == KIND_SPARK ) {
		// hot at the head, thin toward the tail; the metal cools from white to red over its life
		const float across = exp( -p.y * p.y * 6.0 ) * saturate( 1.0 - abs( p.y ) );
		const float along = input.texcoord.x * input.texcoord.x * saturate( ( 1.0 - input.texcoord.x ) * 12.0 );
		const float cooled = heat * ( 1.0 - 0.75 * age );
		const float3 body = ( heat > 0.0 ) ? HeatColor( cooled ) * 4.0 : lerp( input.light.rgb, input.light.rgb + 1.0, 1.0 - age );
		radiance = body * ( across * along ) * ( ( heat > 0.0 ) ? dot( input.light.rgb, 0.3333 ) : 1.0 );
	} else if ( kind == KIND_MARK ) {
		// a ragged spot whose rim cools first
		const float ragged = Fbm( p * 2.5 + float( seed ), seed );
		const float body = saturate( 1.15 - r * ( 1.0 + 0.9 * ragged ) );
		const float cooled = saturate( heat * ( 1.0 - age ) * ( 0.35 + body ) * ( 0.8 + 0.4 * Fbm( p * 6.0, seed + 7 ) ) );
		radiance = HeatColor( cooled ) * 4.0 * smoothstep( 0.0, 0.25, body ) * dot( input.light.rgb, 0.3333 );
	} else if ( kind == KIND_FLASH ) {
		// rays by angle around a core
		const float angle = atan2( p.y, p.x ) * ( 1.0 / 6.2831853 ) + 0.5;
		const float rays = ValueNoise( float2( angle * 14.0, float( seed ) ), seed ) * ValueNoise( float2( angle * 5.0 + 3.0, float( seed ) ), seed + 3 );
		const float core = exp( -r * r * 14.0 );
		const float spikes = rays * exp( -r * 3.5 ) * 1.6;
		const float shape = ( core + spikes ) * saturate( ( 1.0 - r ) * 4.0 );
		radiance = ( input.light.rgb + core * 0.8 * dot( input.light.rgb, 0.3333 ) ) * shape;
	} else if ( kind == KIND_ORB ) {
		// a ball of plasma marched through as a volume; heat: the BFG's, a deeper breath and arcs that reach further
		const float t = input.time + float( seed ) * 7.31;
		const float breath = sin( t * 2.4 ) * ( 0.04 + 0.1 * heat );
		const float pulse = ValueNoise( float2( t * 11.0, 0.5 ), seed + 11 );
		const float radius = ( 0.3 + 0.012 * pulse ) * ( 1.0 + breath ) * ( 1.0 - 0.2 * heat );
		const float2 s = p / radius;
		const float d = r / radius;
		const float white = dot( input.light.rgb, 0.3333 );

		// emission summed along the eye's ray: thin filaments of a warped noise that turn, a core hot by the inverse square
		float glow = 0.0;
		[branch] if ( d < 1.0 ) {
			const float halfDepth = sqrt( 1.0 - d * d );
			const float jitter = Hash21( int2( input.position.xy ), seed );
			const float2 spin = float2( cos( t * 0.6 ), sin( t * 0.6 ) );
			for ( int i = 0; i < ORB_STEPS; i++ ) {
				const float z = ( ( float( i ) + jitter ) * ( 2.0 / ORB_STEPS ) - 1.0 ) * halfDepth;
				const float rr = d * d + z * z;
				const float3 q = float3( s.x * spin.x + z * spin.y, s.y, z * spin.x - s.x * spin.y );
				const float3 warp = float3( ValueNoise3( q * 1.6 + float3( 0.0, t * 0.5, 0.0 ), seed + 3 ),
					ValueNoise3( q * 1.6 + float3( 31.0, 0.0, t * 0.4 ), seed + 4 ), 0.5 ) - 0.5;
				const float n = Fbm3( q * 2.4 + warp * 1.8 + float3( 0.0, -t * 0.7, 0.0 ), seed + 6 );
				const float ridge = saturate( 1.0 - abs( n * 2.0 - 1.0 ) * 2.5 );
				glow += ridge * ridge * ( 1.0 - rr ) * ( 3.0 - 2.0 * rr ) + 0.012 / ( rr + 0.012 );
			}
			glow *= halfDepth * ( 2.0 / ORB_STEPS );
		}
		// the colour only where it is dim: the hot parts go white; around it a glare by the inverse square
		const float glare = 0.03 / ( d * d * 0.5 + 0.03 );
		radiance = input.light.rgb * ( glow * 2.5 + glare * 0.5 ) + white * ( glow * glow * 6.0 + glare * glare );

		// arcs: each strikes out at its own angle, a fractal line with a fork, fades in a fraction of a second
		const float angle = atan2( p.y, p.x );
		const int count = 3 + int( heat * 3.0 + 0.5 );
		float hot = 0.0;
		float halo = 0.0;
		for ( int a = 0; a < count; a++ ) {
			const float clock = t * ( 4.0 + float( a ) * 1.3 ) + float( a ) * 0.37;
			const float strike = floor( clock );
			const uint id = seed + uint( a ) * 17u + uint( fmod( strike, 1024.0 ) ) * 131u;
			const float reach = ( 1.5 + 2.0 * Hash21( int2( a, 1 ), id ) ) * ( 1.0 + 1.2 * heat );
			const float along = saturate( ( d - 0.9 ) / ( reach - 0.9 ) );
			// half the displacement at twice the detail, octave by octave
			float bend = 0.0;
			float fork = 0.0;
			float amplitude = 1.0;
			float x = d * 0.9;
			for ( int octave = 0; octave < ARC_OCTAVES; octave++ ) {
				bend += amplitude * ( ValueNoise( float2( x, float( a ) ), id + uint( octave ) ) - 0.5 );
				fork += amplitude * ( ValueNoise( float2( x, float( a ) + 40.0 ), id + uint( octave ) ) - 0.5 );
				amplitude *= 0.5;
				x *= 2.13;
			}
			float turn = angle - Hash21( int2( a, 0 ), id ) * 6.2831853;
			turn -= 6.2831853 * round( turn * ( 1.0 / 6.2831853 ) );
			const float swing = 0.45 * radius * reach;
			const float side = sin( turn ) * r - bend * swing * sqrt( along );
			// the fork leaves the arc a third of the way out, to one side, thinner
			const float forked = max( along - 0.33, 0.0 );
			const float forkSide = side - ( ( Hash21( int2( a, 3 ), id ) < 0.5 ) ? -1.0 : 1.0 ) * forked * swing * 1.4 - ( fork - bend ) * swing * forked;
			const float width = 0.0035 * ( 1.0 - 0.6 * along );
			// half of the strikes are skipped: a few arcs at a time
			const float live = ( abs( turn ) < 1.2 ) ? step( 0.5, Hash21( int2( a, 2 ), id ) ) : 0.0;
			const float envelope = live * exp( -( clock - strike ) * 7.0 ) * step( 0.85, d ) * saturate( ( reach - d ) * 3.0 );
			const float line0 = width / ( abs( side ) + width );
			const float line1 = width * 6.0 / ( abs( side ) + width * 6.0 );
			const float line2 = width * 0.5 / ( abs( forkSide ) + width * 0.5 ) * step( 0.0001, forked );
			hot += ( line0 * line0 + line2 * line2 * 0.6 ) * envelope;
			halo += line1 * line1 * envelope;
		}
		radiance += ( ( input.light.rgb + white * 2.0 ) * hot * 3.0 + input.light.rgb * halo * 0.3 ) * ( 1.0 + heat );
		radiance *= saturate( ( 1.0 - r ) * 5.0 ) * ( 0.85 + 0.3 * pulse );
	} else {
		// a white-hot core in a tinted halo
		const float core = exp( -r * r * 22.0 );
		const float halo = saturate( 1.0 - r );
		const float shape = core + 0.22 * halo * halo * halo;
		const float flicker = 0.9 + 0.2 * ValueNoise( float2( age * 40.0, float( seed ) ), seed );
		radiance = ( input.light.rgb * shape + core * 0.6 * dot( input.light.rgb, 0.3333 ) ) * flicker;
	}
	radiance *= input.light.a;

	// fade where it meets the scene; the weapon's squeezed z counts as far, as in texture_color_soft
	[branch] if ( input.fadeScale > 0.0 ) {
		const float sceneZ = viewDepthMap.Load( int3( int2( input.position.xy ) - int2( rpSoftParticle.zw ), 0 ) ).x;
		const bool weapon = sceneZ * 2.0 - 1.0 <= WEAPON_DEPTH_SCALE * -rpProjectionMatrixZ.z;
		radiance *= weapon ? 1.0 : saturate( ( ViewDepth( sceneZ ) - ViewDepth( input.position.z ) ) * input.fadeScale );
	}

	// radiance into the linear scene, its sRGB encoding into the gamma phase and into an LDR frame
	if ( !HdrPhase() ) {
		radiance = LinearToSRGB( GammaPhase() ? radiance : saturate( radiance ) );
	}
	return SceneOutput( float4( radiance, 1.0 ) );
}
