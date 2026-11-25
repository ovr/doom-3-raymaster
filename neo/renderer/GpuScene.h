// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

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
	out only when it differs from what the GPU holds at its index. Only the main view sends; a frame that is not sent
	leaves a gap in the sequence, which the backend refuses. A full update (the first, a new set of static buffers,
	a refused update) drops every slot and sends the records of the frame's instances. Out for now: geometry of the frame's
	vertex cache (particles, deforms, decals, CPU skinning), translucent surfaces, the weapon and the player's body.
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

// One surface of an instance as include/gpuScene.hlsli reads it. On the GPU textures[] are the backend's view
// indices; shared code sends the textureHandle_t of each beside the record (gpuSceneWrite_t).
struct gpuSceneGeometry_t {
	static constexpr uint32	GEO_OPAQUE = 1u << 0;		// no alpha test: any hit is the hit
	static constexpr uint32	GEO_SKINNED = 1u << 1;		// the vertices are the instance's skinned ones of the frame
	static constexpr uint32	GEO_TWO_SIDED = 1u << 2;
	static constexpr uint32	GEO_NO_SHADING = 1u << 3;	// an occluder only: a GUI, a mirror
	static constexpr uint32	GEO_CASTS_SHADOW = 1u << 4;

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

// A record of this frame: the material's images become textures on the main thread.
struct gpuSceneWrite_t {
	int						index;		// of the geometry record
	gpuSceneGeometry_t		record;
	const idMaterial *		material;	// EnsureNotPurged before its images are read
	idImage *				images[GPU_SCENE_TEXTURES];
	textureHandle_t			textures[GPU_SCENE_TEXTURES];	// filled by the back end
};

// An entity in this frame's acceleration structure.
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
	uint32					sequence;			// one more than the frame before of the scene
	bool					full;				// every slot was dropped: the writes are the whole scene
	int						staticGeneration;	// idVertexCache's: the static buffers the offsets point into
	int						geometryCapacity;	// records the buffer holds; it only grows
	gpuSceneWrite_t *		writes;				// in index order
	int						numWrites;
	gpuSceneInstance_t *	instances;
	int						numInstances;
	int						numGeometries;		// records in live slots, for r_rtStats
};

// What UpdateGpuScene gets: the frame with its static buffers.
struct gpuSceneUpdate_t {
	const gpuSceneFrame_t *	frame;
	bufferHandle_t			staticVertexBuffer;
	bufferHandle_t			staticIndexBuffer;
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
	// The main view after R_AddModels: this frame's update into frame memory and view.gpuScene. A second view of
	// the same frame gets the same update.
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
		int					skinnedVerts = 0;
		uint64				blasKey = 0;
		bool				animated = false;		// a material that is not constant: built again every frame
		bool				skinned = false;
	};

	void					BuildSlot( slot_t & slot, const idRenderEntityLocal & def, const idRenderModel & model, bool skinned );
	void					FreeSlot( slot_t & slot );
	void					Emit( const gpuSceneWrite_t & write );	// a write only when it differs from the held record

	struct sent_t {
		gpuSceneGeometry_t	record;
		const idMaterial *	material = nullptr;
		idImage *			images[GPU_SCENE_TEXTURES] = {};
		bool				valid = false;
	};

	idList<slot_t>			slots;				// by entityDef index
	idList<sent_t>			sent;				// by geometry index: what the GPU holds since the last full update
	idGpuSceneRanges		geometries;
	idList<gpuSceneWrite_t>	writes;				// this frame's, before the copy into frame memory
	idList<gpuSceneInstance_t>	instances;
	idList<int>				seenFrame;			// by slot: the frame it was last taken in
	uint32					id;
	uint32					sequence = 0;
	int						staticGeneration = -1;
	bool					full = true;
	int						gatherFrame = -1;
	gpuSceneFrame_t *		gathered = nullptr;	// the frame's update, while gatherFrame is the frame
};

// The back end's half, on the main thread: textures and joints, then the backend; a failure asks for a full one.
void RB_UpdateGpuScene( gpuSceneFrame_t & frame );
