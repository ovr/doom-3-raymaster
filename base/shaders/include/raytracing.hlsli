// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

/*
	R15 on the GPU: what a ray of the GPU scene hits. Everything is read through ResourceDescriptorHeap:

	  TLAS        InstanceID the instance's first geometry record, InstanceIndex its row of the instance table,
	              GeometryIndex the record after the first; mask bit 0 rays that see, bit 1 shadow rays; the front
	              face is the side Doom 3 draws, and only a two-sided geometry is hit from behind
	  records     GpuScene.h's gpuSceneGeometry_t, 288 bytes; textures[] are SRV indices of 2D views
	  instances   d3d12RtInstance_t, 16 bytes: first record, count, the bytes of its skinned vertices, scene instance
	  vertices    idDrawVert, 32 bytes: xyz, st as halves, normal and tangent as bytes, two colours; a skinned
	              geometry's are the frame's skinned ones (internal/rtSkin.hlsl), the others the static buffer's
	  indexes     16-bit, from the geometry's first vertex

	A geometry the BLAS holds not opaque is perforated, alpha-tested as the depth pass tests it (alpha texture times
	the stage's colour alpha against the reference), or casts no shadow: the rays that see take it whole, the shadow
	rays (RtOccluded) skip it. Heap indices that differ between lanes go through NonUniformResourceIndex.
*/

#ifndef RAYTRACING_HLSLI
#define RAYTRACING_HLSLI

#include "ycocg.hlsli"

static const uint RT_GEO_OPAQUE = 1u << 0;
static const uint RT_GEO_SKINNED = 1u << 1;
static const uint RT_GEO_TWO_SIDED = 1u << 2;
static const uint RT_GEO_NO_SHADING = 1u << 3;
static const uint RT_GEO_CASTS_SHADOW = 1u << 4;

// gpuSceneTexture_t
static const uint RT_TEXTURE_DIFFUSE = 0;
static const uint RT_TEXTURE_BUMP = 1;
static const uint RT_TEXTURE_SPECULAR = 2;
static const uint RT_TEXTURE_EMISSIVE = 3;
static const uint RT_TEXTURE_ALPHA = 4;
static const uint RT_TEXTURES = 5;

static const uint RT_MASK_VISIBLE = 1u << 0;
static const uint RT_MASK_SHADOW = 1u << 1;
static const uint RT_NOT_SKINNED = 0xFFFFFFFF;
static const uint RT_GEOMETRY_BYTES = 288;
static const uint RT_INSTANCE_BYTES = 16;
static const uint RT_VERTEX_BYTES = 32;

// The heap indices of what a hit reads.
struct RtHeap {
	uint tlas;
	uint records;
	uint instances;
	uint vertices;		// the static vertex buffer
	uint indexes;		// the static index buffer
	uint skinned;		// the frame's skinned vertices
};

struct RtGeometry {
	uint	vertexOffset;
	uint	indexOffset;
	uint	numIndexes;
	uint	numVerts;
	uint	flags;
	uint	skinnedOffset;
	uint	textures[RT_TEXTURES];
	float4	colors[3];			// diffuse, specular, emissive
	float4	matrices[RT_TEXTURES][2];	// the S and T rows of each texture's stage matrix
	float4	alphaTest;			// x the reference, y the alpha stage's colour alpha
};

struct RtInstance {
	uint	firstGeometry;
	uint	numGeometries;
	uint	skinnedOffset;
	uint	sceneInstance;
};

RtGeometry RtLoadGeometry( RtHeap heap, uint index ) {
	ByteAddressBuffer records = ResourceDescriptorHeap[heap.records];
	const uint at = index * RT_GEOMETRY_BYTES;
	RtGeometry g;
	const uint4 head = records.Load4( at );
	const uint2 flags = records.Load2( at + 16 );
	const uint4 textures = records.Load4( at + 32 );
	g.vertexOffset = head.x;
	g.indexOffset = head.y;
	g.numIndexes = head.z;
	g.numVerts = head.w;
	g.flags = flags.x;
	g.skinnedOffset = flags.y;
	g.textures[0] = textures.x;
	g.textures[1] = textures.y;
	g.textures[2] = textures.z;
	g.textures[3] = textures.w;
	g.textures[4] = records.Load( at + 48 );
	[unroll]
	for ( uint c = 0; c < 3; c++ ) {
		g.colors[c] = asfloat( records.Load4( at + 64 + c * 16 ) );
	}
	[unroll]
	for ( uint t = 0; t < RT_TEXTURES; t++ ) {
		g.matrices[t][0] = asfloat( records.Load4( at + 112 + t * 32 ) );
		g.matrices[t][1] = asfloat( records.Load4( at + 128 + t * 32 ) );
	}
	g.alphaTest = asfloat( records.Load4( at + 272 ) );
	return g;
}

RtInstance RtLoadInstance( RtHeap heap, uint index ) {
	ByteAddressBuffer table = ResourceDescriptorHeap[heap.instances];
	const uint4 row = table.Load4( index * RT_INSTANCE_BYTES );
	RtInstance instance;
	instance.firstGeometry = row.x;
	instance.numGeometries = row.y;
	instance.skinnedOffset = row.z;
	instance.sceneInstance = row.w;
	return instance;
}

// VERTEX_BYTE_TO_FLOAT per component.
float3 RtDecodeDirection( uint packed ) {
	return float3( packed & 0xFF, ( packed >> 8 ) & 0xFF, ( packed >> 16 ) & 0xFF ) * ( 2.0 / 255.0 ) - 1.0;
}

// id's dot4: a texture coordinate is ( s, t, 0, 1 ) against a stage matrix row.
float RtDot4( float2 st, float4 row ) {
	return dot( float4( st, 0.0, 1.0 ), row );
}

// The tangent's sign in its fourth byte, as interaction.hlsl reads it.
float RtDecodeSign( uint packed ) {
	return float( packed >> 24 ) * ( 2.0 / 255.0 ) - 1.0;
}

// The hit point of a triangle: world space, the normals toward what Doom 3 shades; the object space of the
// interactions, whose vectors are the vertices' interpolated and not normalized, as the vertex program passes them.
struct RtSurface {
	float3	position;
	float3	normal;			// the interpolated vertex normal
	float3	faceNormal;		// the triangle's plane, toward the side Doom 3 draws
	float3	weights;		// the barycentric weights of the three vertices
	float2	st[3];			// the vertices' texture coordinates
	float	worldArea;		// of the triangle
	float3	objectPosition;
	float3	objectNormal;
	float3	objectTangent;
	float3	objectBinormal;
};

RtSurface RtSurfaceOf( RtHeap heap, RtGeometry g, uint instanceIndex, uint primitive, float2 barycentrics, float3x4 objectToWorld,
	float3x4 worldToObject ) {
	uint vertexBase = g.vertexOffset;
	uint vertexSrv = heap.vertices;
	if ( ( g.flags & RT_GEO_SKINNED ) != 0 ) {
		vertexBase = RtLoadInstance( heap, instanceIndex ).skinnedOffset + g.skinnedOffset;
		vertexSrv = heap.skinned;
	}
	ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex( vertexSrv )];
	ByteAddressBuffer indexes = ResourceDescriptorHeap[heap.indexes];

	RtSurface s;
	s.weights = float3( 1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y );
	float3 object[3];
	float3 world[3];
	s.objectNormal = float3( 0.0, 0.0, 0.0 );
	s.objectTangent = float3( 0.0, 0.0, 0.0 );
	s.objectBinormal = float3( 0.0, 0.0, 0.0 );
	[unroll]
	for ( uint i = 0; i < 3; i++ ) {
		const uint indexAt = g.indexOffset + ( primitive * 3 + i ) * 2;
		const uint word = indexes.Load( indexAt & ~3u );
		const uint index = ( ( indexAt & 2u ) != 0 ) ? ( word >> 16 ) : ( word & 0xFFFF );
		const uint at = vertexBase + index * RT_VERTEX_BYTES;
		const uint4 head = vertices.Load4( at );
		const uint2 frame = vertices.Load2( at + 16 );
		object[i] = asfloat( head.xyz );
		world[i] = mul( objectToWorld, float4( object[i], 1.0 ) );
		s.st[i] = float2( f16tof32( head.w & 0xFFFF ), f16tof32( head.w >> 16 ) );
		const float3 normal = RtDecodeDirection( frame.x );
		const float3 tangent = RtDecodeDirection( frame.y );
		s.objectNormal += normal * s.weights[i];
		s.objectTangent += tangent * s.weights[i];
		s.objectBinormal += cross( normal, tangent ) * RtDecodeSign( frame.y ) * s.weights[i];
	}
	s.objectPosition = object[0] * s.weights.x + object[1] * s.weights.y + object[2] * s.weights.z;
	s.position = world[0] * s.weights.x + world[1] * s.weights.y + world[2] * s.weights.z;
	// mul( n, worldToObject ): the inverse transpose. The plane of idPlane::FromPoints, ( v0 - v1 ) x ( v2 - v1 ), in
	// object space as DXR's facing is, so a mirroring transform keeps the side
	s.normal = normalize( mul( s.objectNormal, (float3x3)worldToObject ) );
	s.faceNormal = normalize( mul( cross( object[0] - object[1], object[2] - object[1] ), (float3x3)worldToObject ) );
	s.worldArea = 0.5 * length( cross( world[1] - world[0], world[2] - world[0] ) );
	return s;
}

// A vertex's coordinates of the texture in slot, through its stage matrix.
float2 RtVertexTexCoord( RtGeometry g, uint slot, float2 st ) {
	return float2( RtDot4( st, g.matrices[slot][0] ), RtDot4( st, g.matrices[slot][1] ) );
}

float2 RtTexCoord( RtGeometry g, uint slot, RtSurface s ) {
	return RtVertexTexCoord( g, slot, s.st[0] ) * s.weights.x + RtVertexTexCoord( g, slot, s.st[1] ) * s.weights.y
		+ RtVertexTexCoord( g, slot, s.st[2] ) * s.weights.z;
}

// The level of a texture sampled at the hit: texels per world unit of the triangle in this texture's coordinates,
// times footprint, the world units one pixel covers there.
float4 RtSample( RtGeometry g, uint slot, RtSurface s, float footprint, uint samplerIndex ) {
	Texture2D<float4> image = ResourceDescriptorHeap[NonUniformResourceIndex( g.textures[slot] )];
	SamplerState linearSampler = SamplerDescriptorHeap[samplerIndex];
	float2 uv[3];
	[unroll]
	for ( uint i = 0; i < 3; i++ ) {
		uv[i] = RtVertexTexCoord( g, slot, s.st[i] );
	}
	const float2 at = uv[0] * s.weights.x + uv[1] * s.weights.y + uv[2] * s.weights.z;
	uint width, height, levels;
	image.GetDimensions( 0, width, height, levels );
	const float2 d1 = uv[1] - uv[0];
	const float2 d2 = uv[2] - uv[0];
	const float uvArea = 0.5 * abs( d1.x * d2.y - d1.y * d2.x );
	const float texelsPerUnit = sqrt( uvArea * float( width ) * float( height ) / max( s.worldArea, 1e-12 ) );
	const float level = log2( max( footprint * texelsPerUnit, 1e-8 ) );
	return image.SampleLevel( linearSampler, at, level );
}

// footprint: world units of a pixel across the surface, the cone's width stretched by the incidence (at most 4x).
float RtFootprint( float t, float pixelSpread, float3 direction, float3 faceNormal ) {
	return t * pixelSpread / max( abs( dot( direction, faceNormal ) ), 0.25 );
}

// The diffuse as interaction.hlsl reads it, YCoCg decoded, times its stage's colour; in gamma, as the texture.
float3 RtDiffuse( RtGeometry g, RtSurface s, float footprint, uint samplerIndex ) {
	return ConvertYCoCgToRGB( RtSample( g, RT_TEXTURE_DIFFUSE, s, footprint, samplerIndex ) ) * g.colors[0].rgb;
}

// A candidate the BLAS holds not opaque, as the ray sees it: the side it is drawn from; then whole, or alpha-tested.
// A shadow ray takes the side that faces the light, the back face from where it starts, and nothing that casts no
// shadow.
bool RtCandidatePasses( RtHeap heap, uint record, uint instanceIndex, uint primitive, float2 barycentrics, bool frontFace,
	float3x4 objectToWorld, float3x4 worldToObject, float t, float pixelSpread, float3 direction, uint samplerIndex, bool shadow ) {
	const RtGeometry g = RtLoadGeometry( heap, record );
	if ( ( shadow ? frontFace : !frontFace ) && ( g.flags & RT_GEO_TWO_SIDED ) == 0 ) {
		return false;
	}
	if ( shadow && ( g.flags & RT_GEO_CASTS_SHADOW ) == 0 ) {
		return false;
	}
	if ( ( g.flags & RT_GEO_OPAQUE ) != 0 ) {
		return true;
	}
	const RtSurface s = RtSurfaceOf( heap, g, instanceIndex, primitive, barycentrics, objectToWorld, worldToObject );
	const float alpha = RtSample( g, RT_TEXTURE_ALPHA, s, RtFootprint( t, pixelSpread, direction, s.faceNormal ), samplerIndex ).a;
	return alpha * g.alphaTest.y >= g.alphaTest.x;
}

struct RtHit {
	bool		hit;
	float		t;
	uint		instanceId;		// the instance's first record
	uint		instanceIndex;	// its row of the instance table
	uint		record;			// instanceId + GeometryIndex
	uint		primitive;
	float2		barycentrics;
	bool		frontFace;		// the side Doom 3 draws
	float3x4	objectToWorld;
	float3x4	worldToObject;
};

// The nearest hit of instances in mask, perforated geometry alpha-tested; pixelSpread: radians per pixel of the cone.
RtHit RtTrace( RtHeap heap, RayDesc ray, uint mask, float pixelSpread, uint samplerIndex ) {
	RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[heap.tlas];
	// AMD's driver gives a committed non-opaque hit the candidate's t plus TMin (Gpu_RtHit): the query starts at
	// TMin from there with a TMin of 0, and t counts from the ray's origin again after it
	const float start = ray.TMin;
	RayDesc from = ray;
	from.Origin = ray.Origin + ray.Direction * start;
	from.TMin = 0.0;
	from.TMax = ray.TMax - start;
	// back faces are culled as the raster culls them; an instance with a two-sided geometry culls nothing, and
	// the loop rejects the back faces of its one-sided perforated ones
	RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
	query.TraceRayInline( tlas, RAY_FLAG_NONE, mask, from );
	while ( query.Proceed() ) {
		if ( query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
			&& RtCandidatePasses( heap, query.CandidateInstanceID() + query.CandidateGeometryIndex(), query.CandidateInstanceIndex(),
				query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), query.CandidateTriangleFrontFace(),
				query.CandidateObjectToWorld3x4(), query.CandidateWorldToObject3x4(), start + query.CandidateTriangleRayT(),
				pixelSpread, ray.Direction, samplerIndex, false ) ) {
			query.CommitNonOpaqueTriangleHit();
		}
	}
	RtHit hit;
	hit.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
	hit.t = hit.hit ? start + query.CommittedRayT() : ray.TMax;
	hit.instanceId = query.CommittedInstanceID();
	hit.instanceIndex = query.CommittedInstanceIndex();
	hit.record = query.CommittedInstanceID() + query.CommittedGeometryIndex();
	hit.primitive = query.CommittedPrimitiveIndex();
	hit.barycentrics = query.CommittedTriangleBarycentrics();
	hit.frontFace = query.CommittedTriangleFrontFace();
	hit.objectToWorld = query.CommittedObjectToWorld3x4();
	hit.worldToObject = query.CommittedWorldToObject3x4();
	return hit;
}

// Whether a shadow caster lies within tMax along the ray: Doom 3's shadow volumes extrude the sides that face the
// light, which the ray meets from behind, so it culls front faces; any hit ends the search. footprint: the
// world units of a pixel at the origin, for the alpha's level.
bool RtOccluded( RtHeap heap, float3 origin, float3 direction, float tMax, float footprint, uint samplerIndex ) {
	RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[heap.tlas];
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_CULL_FRONT_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline( tlas, RAY_FLAG_NONE, RT_MASK_SHADOW, ray );
	while ( query.Proceed() ) {
		// a cone of the footprint's width all along: the level stays the origin's
		if ( query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
			&& RtCandidatePasses( heap, query.CandidateInstanceID() + query.CandidateGeometryIndex(), query.CandidateInstanceIndex(),
				query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), query.CandidateTriangleFrontFace(),
				query.CandidateObjectToWorld3x4(), query.CandidateWorldToObject3x4(), 1.0, footprint, direction, samplerIndex, true ) ) {
			query.CommitNonOpaqueTriangleHit();
		}
	}
	return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#endif
