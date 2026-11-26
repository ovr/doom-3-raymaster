// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include <algorithm>
#include <span>

#include "RenderBackendTypes.h"

/*
	The GPU scene (R15 of RenderBackend.h): what the ray traced passes of the main view see.

	  front end (game thread)                     back end (main thread)              backend
	  idGpuScene::Gather, after R_AddModels  -->  RB_UpdateGpuScene: images to    --> UpdateGpuScene: the records
	    a slot per entityDef, its surfaces as       textures (EnsureNotPurged),         into its persistent buffer,
	    a range of geometry records; this frame's   joint handles to buffers            the instances for the
	    dirty records and live instances                                                acceleration structures
	    (gpuSceneFrame_t, frame memory)

	A slot is built again when its entity changes (model, parms, a material that is not constant), and a record goes
	out only when it differs from what the GPU holds at its index. A slot's one-sided and two-sided surfaces are an
	instance each, since an instance culls as a whole. A full update (the first, a new set of static buffers, a
	refused update) drops every slot and sends the records of the frame's instances. Out for now: geometry of the
	frame's vertex cache (particles, deforms, decals, CPU skinning), translucent surfaces, the weapon and the
	player's body.

	Every view of a frame that sees other areas gathers again (a portal sky's view comes before the player's): its
	update has its own instances and lights and every write of the frame so far (base, the last sequence of the
	frame before), so whichever one the main view sends applies. A frame that is not sent leaves a gap, which the
	backend refuses.

	Every update also holds all the lights of the same areas, not only the view's (a reflected surface behind the
	eye is lit by lights the view culled): one per enabled light stage, without fog and blend lights.
*/

class idImage;
class idMaterial;
class idRenderEntityLocal;
class idRenderModel;
class idRenderWorldLocal;
struct viewDef_t;

// The textures of a geometry record, in its textures[] order.
enum class gpuSceneTexture_t : uint8 { DIFFUSE, BUMP, SPECULAR, EMISSIVE, ALPHA, COUNT };
constexpr int GPU_SCENE_TEXTURES = static_cast<int>( gpuSceneTexture_t::COUNT );

// One surface of an instance as include/raytracing.hlsli reads it. On the GPU textures[] are the backend's view
// indices; shared code sends the textureHandle_t of each beside the record (gpuSceneWrite_t).
struct gpuSceneGeometry_t {
	static constexpr uint32	GEO_OPAQUE = 1u << 0;		// no alpha test: any hit is the hit
	static constexpr uint32	GEO_SKINNED = 1u << 1;		// the vertices are the instance's skinned ones of the frame
	static constexpr uint32	GEO_TWO_SIDED = 1u << 2;
	static constexpr uint32	GEO_NO_SHADING = 1u << 3;	// an occluder only: a GUI, a mirror
	static constexpr uint32	GEO_CASTS_SHADOW = 1u << 4;	// without it the BLAS holds the geometry not opaque: shadow rays skip it

	uint32		vertexOffset;		// bytes into the static vertex buffer: idDrawVert
	uint32		indexOffset;		// bytes into the static index buffer: triIndex_t
	uint32		numIndexes;
	uint32		numVerts;
	uint32		flags;				// GEO_*
	uint32		skinnedOffset;		// GEO_SKINNED: the bytes of the instance's skinned vertices before this geometry's
	uint32		unused[2];
	uint32		textures[8];		// gpuSceneTexture_t, then unused
	idVec4		colors[3];			// diffuse, specular, emissive: the stage colours, clamped as the interactions clamp them
	idVec4		matrices[GPU_SCENE_TEXTURES][2];	// the S and T rows of each texture's stage matrix
	idVec4		alphaTest;			// x the reference, y the alpha stage's colour alpha
};
static_assert( sizeof( gpuSceneGeometry_t ) == 288 );

// A light stage as include/rtLighting.hlsli reads it, in world space; projection and falloff are the backend's views
// on the GPU, beside the record shared code sends their textureHandle_t (gpuSceneLightWrite_t).
struct gpuSceneLight_t {
	static constexpr uint32	LIGHT_SHADOWS = 1u << 0;	// LightCastsShadows
	static constexpr uint32	LIGHT_AMBIENT = 1u << 1;	// interactionAmbient's fixed direction, no shadows

	idVec4		origin;				// xyz globalLightOrigin
	idVec4		project[4];			// the S, T and Q planes of the projection, the stage's texture matrix baked in; the falloff's S
	idVec4		color;				// r_lightScale times the stage's colour: RB_RenderInteractions' lightColor, in gamma
	idVec4		boundsMin;			// globalLightBounds
	idVec4		boundsMax;
	uint32		projection;
	uint32		falloff;
	uint32		flags;				// LIGHT_*
	uint32		unused;
};
static_assert( sizeof( gpuSceneLight_t ) == 144 );

struct gpuSceneLightWrite_t {
	gpuSceneLight_t			light;
	const idMaterial *		material;	// the light's: EnsureNotPurged
	idImage *				images[2];	// projection, falloff
	textureHandle_t			textures[2];	// filled by the back end
};

// A record of this frame: the material's images become textures on the main thread.
struct gpuSceneWrite_t {
	int						index;		// of the geometry record
	gpuSceneGeometry_t		record;
	const idMaterial *		material;	// EnsureNotPurged before its images are read
	idImage *				images[GPU_SCENE_TEXTURES];
	textureHandle_t			textures[GPU_SCENE_TEXTURES];	// filled by the back end
};

// An entity's one-sided or two-sided surfaces in this frame's acceleration structure: its geometries cull alike.
struct gpuSceneInstance_t {
	static constexpr uint32	MASK_VISIBLE = 1u << 0;		// reflection and debug rays
	static constexpr uint32	MASK_SHADOW = 1u << 1;		// shadow rays

	float					transform[3][4];	// object to world, the rows of the drawn model matrix
	uint32					firstGeometry;
	uint32					numGeometries;
	uint32					mask;
	uint32					skinnedVerts;		// a skinned instance: the vertices of its geometries, else 0
	uint64					blasKey;			// equal keys, equal positions and opacity: one BLAS; 0 a skinned one of its own
	vertCacheHandle_t		jointCache;			// skinned: this frame's joints
	int						numJoints;
	bufferHandle_t			jointBuffer;		// filled by the back end from jointCache
	int						jointOffset;
};

// What a frame sends (frame memory; the back end fills the textures and joint buffers).
struct gpuSceneFrame_t {
	uint32					sceneId;			// of the render world's scene
	uint32					sequence;			// one more per gather of the scene
	uint32					base;				// the update the writes follow: the last one of the frame before
	bool					full;				// every slot was dropped: the writes are the whole scene
	int						staticGeneration;	// idVertexCache's: the static buffers the offsets point into
	int						geometryCapacity;	// records the buffer holds; it only grows
	gpuSceneWrite_t *		writes;				// in index order
	int						numWrites;
	gpuSceneInstance_t *	instances;
	int						numInstances;
	int						numGeometries;		// records in live slots, for r_rtStats
	gpuSceneLightWrite_t *	lights;				// all of the frame's
	int						numLights;
};

// What UpdateGpuScene gets: the frame with its static buffers.
struct gpuSceneUpdate_t {
	const gpuSceneFrame_t *	frame;
	bufferHandle_t			staticVertexBuffer;
	bufferHandle_t			staticIndexBuffer;
};

// What the backend made of an update's acceleration structures, for r_rtStats.
struct gpuSceneStats_t {
	int						tlasInstances = 0;
	int						waiting = 0;		// instances left out: their BLAS waits for the frame's build budget
	int						blas = 0;			// static BLAS kept, one per distinct model
	int						built = 0;			// static BLAS built this frame
	int						compacted = 0;		// static BLAS copied at their compacted size this frame
	int						skinned = 0;		// skinned instances, skinned and built again every frame
	int						skinnedVerts = 0;
	uint64					blasBytes = 0;		// of the static BLAS
	uint64					frameBytes = 0;		// the TLAS, the skinned BLAS and vertices, the scratch
};

// A slot's records in the order of its instances: the one-sided, then the two-sided, each half counting its skinned
// vertices from 0 into skinnedVerts. Returns how many are one-sided.
[[nodiscard]] inline int GpuScene_OrderBySide( std::span<gpuSceneWrite_t> writes, int skinnedVerts[2] ) {
	const auto oneSided = []( const gpuSceneWrite_t & write ) { return ( write.record.flags & gpuSceneGeometry_t::GEO_TWO_SIDED ) == 0; };
	const int numOneSided = static_cast<int>( std::stable_partition( writes.begin(), writes.end(), oneSided ) - writes.begin() );
	skinnedVerts[0] = skinnedVerts[1] = 0;
	for ( int i = 0; i < static_cast<int>( writes.size() ); i++ ) {
		gpuSceneGeometry_t & record = writes[i].record;
		int & verts = skinnedVerts[( i < numOneSided ) ? 0 : 1];
		record.skinnedOffset = static_cast<uint32>( verts * sizeof( idDrawVert ) );
		verts += ( record.flags & gpuSceneGeometry_t::GEO_SKINNED ) != 0 ? static_cast<int>( record.numVerts ) : 0;
	}
	return numOneSided;
}

// The key of a static BLAS of these records: their positions and what the BLAS holds opaque. Never 0, a skinned one's.
[[nodiscard]] inline uint64 GpuScene_BlasKey( std::span<const gpuSceneWrite_t> writes ) {
	uint64 key = 0xcbf29ce484222325ull;
	const auto hash = [&key]( uint32 value ) {
		for ( int i = 0; i < 4; i++ ) {
			key ^= ( value >> ( i * 8 ) ) & 0xff;
			key *= 0x100000001b3ull;
		}
	};
	for ( const gpuSceneWrite_t & write : writes ) {
		const gpuSceneGeometry_t & record = write.record;
		hash( record.vertexOffset );
		hash( record.indexOffset );
		hash( record.numIndexes );
		hash( record.numVerts );
		hash( record.flags & ( gpuSceneGeometry_t::GEO_OPAQUE | gpuSceneGeometry_t::GEO_CASTS_SHADOW ) );
	}
	return key | 1;
}

// Which refused updates ask the front end for a full one: a scene's first and a refused full one. The deltas gathered
// before the front end saw the request are refused too; asking again would send a second full update.
struct gpuSceneRecovery_t {
	uint32					awaiting = 0;		// the scene whose full update is asked for and not applied yet

	[[nodiscard]] bool		Asks( uint32 sceneId, bool full, bool applied ) {
		if ( applied ) {
			awaiting = ( full && awaiting == sceneId ) ? 0 : awaiting;
			return false;
		}
		if ( !full && awaiting == sceneId ) {
			return false;
		}
		awaiting = sceneId;
		return true;
	}
};

// First fit over [0, capacity), free ranges sorted and merged. A failed fit grows the capacity (at least double).
class idGpuSceneRanges {
public:
	[[nodiscard]] int		Allocate( int count );
	void					Free( int first, int count );
	void					Clear() { free.Clear(); capacity = 0; used = 0; }
	[[nodiscard]] int		Capacity() const { return capacity; }
	[[nodiscard]] int		Used() const { return used; }
	[[nodiscard]] int		NumFreeRanges() const { return free.Num(); }

private:
	struct range_t {
		int					first;
		int					count;
	};
	idList<range_t>			free;		// sorted by first, never adjacent
	int						capacity = 0;
	int						used = 0;
};

inline int idGpuSceneRanges::Allocate( int count ) {
	assert( count > 0 );
	for ( int i = 0; i < free.Num(); i++ ) {
		range_t & range = free[i];
		if ( range.count >= count ) {
			const int first = range.first;
			range.first += count;
			range.count -= count;
			if ( range.count == 0 ) {
				free.RemoveIndex( i );
			}
			used += count;
			return first;
		}
	}
	// a free range at the end grows into the new capacity
	int first = capacity;
	if ( free.Num() > 0 && free[free.Num() - 1].first + free[free.Num() - 1].count == capacity ) {
		first = free[free.Num() - 1].first;
		free.RemoveIndex( free.Num() - 1 );
	}
	const int grown = Max( Max( capacity * 2, 1024 ), first + count );
	if ( first + count < grown ) {
		free.Append( { first + count, grown - first - count } );
	}
	capacity = grown;
	used += count;
	return first;
}

inline void idGpuSceneRanges::Free( int first, int count ) {
	if ( count <= 0 ) {
		return;
	}
	assert( first >= 0 && first + count <= capacity );
	int at = 0;
	while ( at < free.Num() && free[at].first < first ) {
		at++;
	}
	free.Insert( { first, count }, at );
	used -= count;
	if ( at + 1 < free.Num() && free[at].first + free[at].count == free[at + 1].first ) {
		free[at].count += free[at + 1].count;
		free.RemoveIndex( at + 1 );
	}
	if ( at > 0 && free[at - 1].first + free[at - 1].count == free[at].first ) {
		free[at - 1].count += free[at].count;
		free.RemoveIndex( at );
	}
}

// One per render world, on the front end's thread.
class idGpuScene {
public:
							idGpuScene();
	// A view after R_AddModels: its update into frame memory and view.gpuScene. A view of the same frame that
	// gathers the same (the second eye) gets the same update.
	void					Gather( idRenderWorldLocal & world, viewDef_t & view );
	void					FreeEntity( int index );	// FreeEntityDef
	void					Reset();					// the next Gather sends a full update
	[[nodiscard]] uint32	Id() const { return id; }

private:
	struct slot_t {
		const idRenderModel *	model = nullptr;	// what the records were built from; nullptr none
		int					modified = 0;			// the entity's lastModifiedFrameNum then
		int					firstGeometry = 0;
		int					numGeometries = 0;
		int					numOneSided = 0;		// the first records; the rest are two-sided: an instance each
		int					skinnedVerts[2] = {};	// of each half
		uint64				blasKey[2] = {};
		bool				animated = false;		// a material that is not constant: built again every frame
		bool				skinned = false;
	};
	// A gather of this frame, by the view it was made for.
	struct gather_t {
		const viewDef_t *	view;
		gpuSceneFrame_t *	out;
	};

	void					BuildSlot( slot_t & slot, const idRenderEntityLocal & def, const idRenderModel & model, bool skinned );
	void					FreeSlot( slot_t & slot );
	void					Emit( const gpuSceneWrite_t & write );	// a write only when it differs from the held record
	void					GatherLights( idRenderWorldLocal & world, const viewDef_t & view );

	struct sent_t {
		gpuSceneGeometry_t	record;
		const idMaterial *	material = nullptr;
		idImage *			images[GPU_SCENE_TEXTURES] = {};
		bool				valid = false;
	};

	idList<slot_t>			slots;				// by entityDef index
	idList<sent_t>			sent;				// by geometry index: what the GPU holds since the last full update
	idGpuSceneRanges		geometries;
	idList<gpuSceneWrite_t>	writes;				// this frame's so far, before the copy into frame memory
	idList<gpuSceneWrite_t>	building;			// BuildSlot's records before the slot knows how many it holds
	idList<gpuSceneInstance_t>	instances;
	idList<gpuSceneLightWrite_t>	lights;
	idList<int>				seen;				// by slot: the gather it was last taken in
	idList<int>				lightSeen;			// by lightDef
	idList<gather_t>		gathers;			// of gatherFrame
	uint32					id;
	uint32					sequence = 0;
	uint32					frameBase = 0;		// the sequence before gatherFrame's first gather
	int						staticGeneration = -1;
	bool					full = true;		// the next gather drops every slot
	bool					frameFull = false;	// a gather of gatherFrame did: its updates are full
	int						gatherFrame = -1;
	int						gatherCount = 0;
};

// The back end's half, on the main thread: textures and joints, then the backend; a failure asks for a full one.
void RB_UpdateGpuScene( gpuSceneFrame_t & frame );
