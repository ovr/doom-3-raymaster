// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The GPU scene's persistent half (R15): the geometry records in a DEFAULT buffer with a raw view, kept up to date
	by the writes of each update, and a CPU copy for the acceleration structures. The buffer only grows; a bigger one
	gets the old records by a GPU copy. A record's textures are the views of SAMPLED textures (the black fallback for
	anything else), so destroying one invalidates the scene until the next full update. The lights are written whole
	by every applied update into a buffer of their own, which only grows too.
*/

#include <array>
#include <map>
#include <span>

#include "../GpuScene.h"
#include "d3d12_Descriptors.h"
#include "d3d12_Memory.h"
#include "d3d12_Textures.h"

class idD3D12GpuScene {
public:
	struct stats_t {
		int							writes = 0;
		int							copies = 0;		// runs of consecutive records
		uint64						bytes = 0;
		bool						full = false;
	};

									idD3D12GpuScene( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12Graveyard & graveyard,
										idD3D12DescriptorPool & srvPool, idD3D12Barriers & barriers, idD3D12Uploader & uploader,
										idD3D12TextureStore & textures );
									idD3D12GpuScene( const idD3D12GpuScene & ) = delete;
	idD3D12GpuScene &				operator=( const idD3D12GpuScene & ) = delete;

	// R15. False: not applied, and nothing may be traced until a full update applies.
	[[nodiscard]] bool				Update( const gpuSceneUpdate_t & update );
	[[nodiscard]] bool				Valid() const { return valid; }
	// The records as the GPU holds them (views in textures[]), by index; entries of no live slot are stale.
	[[nodiscard]] std::span<const gpuSceneGeometry_t>	Geometries() const { return mirror; }
	// The last update's instances and static buffers: this frame's, while Valid.
	[[nodiscard]] std::span<const gpuSceneInstance_t>	Instances() const { return instances; }
	[[nodiscard]] bufferHandle_t	StaticVertexBuffer() const { return staticVertexBuffer; }
	[[nodiscard]] bufferHandle_t	StaticIndexBuffer() const { return staticIndexBuffer; }
	[[nodiscard]] ID3D12Resource *	Buffer() const { return buffer.Get(); }
	[[nodiscard]] d3d12ResourceState_t &	BufferState() { return state; }
	[[nodiscard]] d3d12Descriptor_t	BufferSrv() const { return srv; }	// raw, of every record
	[[nodiscard]] int				Capacity() const { return capacity; }
	// The last update's lights (gpuSceneLight_t, views in projection and falloff); no buffer while there were none.
	[[nodiscard]] ID3D12Resource *	LightBuffer() const { return lightBuffer.Get(); }
	[[nodiscard]] d3d12ResourceState_t &	LightBufferState() { return lightState; }
	[[nodiscard]] d3d12Descriptor_t	LightBufferSrv() const { return lightSrv; }
	[[nodiscard]] int				NumLights() const { return numLights; }
	[[nodiscard]] const stats_t &	LastStats() const { return stats; }
	// Before a pass traces: a texture leaves COPY_DEST only when bound, and a record's may be bound by no draw, or
	// uploaded again (a video) since. Batched transitions of every texture a record names.
	void							MakeTexturesReadable();
	// What was applied last: a build of the acceleration structures follows each new one.
	[[nodiscard]] uint32			SceneId() const { return sceneId; }
	[[nodiscard]] uint32			Sequence() const { return sequence; }
	[[nodiscard]] int				StaticGeneration() const { return staticGeneration; }

private:
	[[nodiscard]] bool				Reserve( int records );
	[[nodiscard]] bool				WriteLights( std::span<const gpuSceneLightWrite_t> lights );
	// The view a record holds of texture; sampled the texture when it is SAMPLED, else NONE (the fallback's view).
	[[nodiscard]] uint32			View( textureHandle_t texture, textureHandle_t & sampled );
	void							SetRecordTexture( size_t record, int slot, textureHandle_t sampled );

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12Graveyard &				graveyard;
	idD3D12DescriptorPool &			srvPool;
	idD3D12Barriers &				barriers;
	idD3D12Uploader &				uploader;
	idD3D12TextureStore &			textures;

	idComPtr<ID3D12Resource>		buffer;
	d3d12ResourceState_t			state;
	d3d12Descriptor_t				srv;
	int								capacity = 0;
	std::vector<gpuSceneGeometry_t>	mirror;
	std::vector<std::array<textureHandle_t, GPU_SCENE_TEXTURES>>	recordTextures;	// by record, the SAMPLED textures it views
	std::map<textureHandle_t, int>	textureUses;	// of recordTextures: what MakeTexturesReadable walks
	std::vector<gpuSceneInstance_t>	instances;
	std::vector<byte>				scratch;

	idComPtr<ID3D12Resource>		lightBuffer;
	d3d12ResourceState_t			lightState;
	d3d12Descriptor_t				lightSrv;
	int								lightCapacity = 0;
	int								numLights = 0;
	std::vector<gpuSceneLight_t>	lightScratch;
	std::vector<textureHandle_t>	lightTextures;	// SAMPLED ones the lights view, for MakeTexturesReadable
	bufferHandle_t					staticVertexBuffer = bufferHandle_t::NONE;
	bufferHandle_t					staticIndexBuffer = bufferHandle_t::NONE;

	bool							valid = false;
	uint32							sceneId = 0;
	uint32							sequence = 0;
	int								staticGeneration = -1;
	uint64							sampledDestroyed = 0;
	stats_t							stats;
};
