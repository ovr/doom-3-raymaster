// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	interaction.hlsl with a traced shadow, between BeginTracedLights and EndTracedLights (R16): the light's
	colour goes to the scene unshadowed, as interaction.hlsl gives it, and to the light target where a caster
	of the GPU scene stands between the pixel and the light. The sum of the second over the sum of the first
	is the part of the pixel's light that the shadows take, whatever the number of lights.

	The light is a disk of radius rpRtLight.w that faces the pixel: one ray per pixel toward a point of it
	(include/rtNoise.hlsli) gives a penumbra once the rays of many pixels or frames are averaged. The ray ends
	a radius short of the disk, so what a lamp is mounted on does not shadow it. Radius 0 is the hard shadow
	of a point, the stencil volumes': the ray starts on the side of the surface that the eye sees and stops at
	the triangles that face away from the light (RtOccluded), so a surface with the light behind it shadows
	itself. While nothing may be traced (rtFlags) the light target gets nothing: the frame stays lit.
	SKINNED=1 is interaction_rt_skinned.
*/

#define INTERACTION_RT 1
#include "include/interaction.hlsli"
#include "include/rtDraw.hlsli"
#include "include/rtNoise.hlsli"

struct PSOutput {
	float4 scene : SV_Target0;
	float4 lights : SV_Target1;
};

PSOutput PSMain( VSOutput input ) {
	// before any branch: the pixel's size on the surface, and the plane the surface lies in
	const float3 dx = ddx( input.worldPosition.xyz );
	const float3 dy = ddy( input.worldPosition.xyz );

	PSOutput output;
	output.scene = SceneOutput( InteractionColor( input ) );
	output.lights = float4( 0.0, 0.0, 0.0, 0.0 );
	if ( !RtDrawValid() || !any( output.scene.rgb > 0.0 ) ) {
		return output;
	}

	// under MSAA a pixel's centre may lie beside its triangle, inside the wall next to it
	const float3 position = EvaluateAttributeCentroid( input.worldPosition ).xyz;
	const float radius = rpRtLight.w;
	const float3 toLight = rpRtLight.xyz - position;
	const float range = length( toLight );
	if ( range <= radius ) {
		return output;
	}
	const float3 axis = toLight / range;
	const float3 other = ( abs( axis.z ) < 0.9 ) ? float3( 0.0, 0.0, 1.0 ) : float3( 1.0, 0.0, 0.0 );
	const float3 right = normalize( cross( other, axis ) );
	const float3 up = cross( axis, right );
	const float2 disk = RtDisk( RtNoise( rtNoise, uint2( input.position.xy ), uint( rpRtShadow.y ) ) ) * radius;
	float3 spread = right * disk.x + up * disk.y;

	// the side of the surface the eye sees
	const float footprint = max( length( dx ), length( dy ) );
	float3 side = cross( dx, dy );
	const float area = length( side );
	side = ( area > 1e-16 ) ? side / area : normalize( input.toEye.xyz );
	side = ( dot( side, input.toEye.xyz ) < 0.0 ) ? -side : side;
	// a surface that faces the light sees its whole disk: a point behind the surface's plane becomes the one across
	if ( dot( toLight, side ) > 0.0 && dot( toLight + spread, side ) <= 0.0 ) {
		spread = -spread;
	}
	const float3 emitter = rpRtLight.xyz + spread;
	// Off the surface on that side, further where a pixel covers more of it. Toward a point behind the surface the
	// ray comes back through the pixel's own point, so the surface stops it and not what lies beside its edge.
	const float3 aim = normalize( emitter - position );
	const float lift = 0.02 + 0.1 * footprint;
	const float rise = dot( aim, side );
	const float3 origin = ( rise > 0.0 ) ? position + side * lift : position - aim * ( lift / max( -rise, 0.05 ) );
	const float3 path = emitter - origin;
	const float reach = length( path );
	if ( reach > radius && RtOccluded( RtDrawHeap(), origin, path / reach, reach - radius, footprint, rtSampler, uint( rpRtShadow.x ) ) ) {
		output.lights = float4( output.scene.rgb, 0.0 );
	}
	return output;
}
