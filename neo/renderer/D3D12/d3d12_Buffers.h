// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_HandleTable.h"
#include "d3d12_Memory.h"

// Where FRAME buffers live is r_d3d12FrameBuffers: 0 UPLOAD, 1 STAGED, 2 GPU_UPLOAD. STATIC ones are always STAGED.
enum class d3d12BufferPlacement_t : uint8 {
	UPLOAD,			// a persistently mapped UPLOAD heap (system memory) the GPU reads in place
	STAGED,			// a DEFAULT heap and a CPU mirror, copied through "staging" at Unmap
	GPU_UPLOAD		// a persistently mapped GPU_UPLOAD heap (video memory the CPU writes, OPTIONS16) read in place
};
[[nodiscard]] const char *			D3D12_PlacementName( d3d12BufferPlacement_t placement );

// What a bufferHandle_t names.
struct d3d12Buffer_t {
	bufferType_t					type = bufferType_t::VERTEX;
	bufferUsage_t					usage = bufferUsage_t::STATIC;
	d3d12BufferPlacement_t			placement = d3d12BufferPlacement_t::UPLOAD;
	int								size = 0;		// what shared code asked for
	uint64							bytes = 0;		// of resource: size + JOINT_TAIL for JOINT
	idComPtr<ID3D12Resource>		resource;
	D3D12_GPU_VIRTUAL_ADDRESS		gpu = 0;
	byte *							cpu = nullptr;	// MapBuffer's pointer, the mapped heap or the mirror; never changes
	idD3D12CpuBlock					mirror;			// STAGED
	d3d12ResourceState_t			state;			// STAGED: decays to COMMON in every list
};

// Vertex, index and joint buffers. Map makes no API call and reads one live record, so it is legal from any
// thread (R1); everything else runs on the owner thread.
class idD3D12BufferStore {
public:
	// the joints' cbuffer is declared whole and a root CBV has no size: gbuffer_skinned's 816 rows (R14)
	static constexpr int			JOINT_TAIL = 816 * 16;

									idD3D12BufferStore( const idD3D12Device & device, idD3D12Graveyard & graveyard, idD3D12Uploader & uploader,
										idD3D12Barriers & barriers, d3d12BufferPlacement_t framePlacement );
	void							CreateDefaults();	// the zero joints and the (0,0,0,1) attribute, after construction
	[[nodiscard]] bufferHandle_t	Create( bufferType_t type, bufferUsage_t usage, int size, const void * data );
	void							Destroy( bufferHandle_t buffer );	// a dead handle is ignored
	[[nodiscard]] void *			Map( bufferHandle_t buffer ) const;
	void							Unmap( bufferHandle_t buffer, int dirtyOffset, int dirtyBytes );	// STAGED: the copy is recorded now
	[[nodiscard]] const d3d12Buffer_t &	Get( bufferHandle_t buffer ) const { return table.Get( buffer ); }
	[[nodiscard]] const d3d12Buffer_t *	Find( bufferHandle_t buffer ) const { return table.Find( buffer ); }
	// For every reader of the open list (a draw, a shader, an acceleration structure build): a STAGED buffer decays
	// to COMMON in every list, so it transitions (batched) into its one read state; the mapped ones are read where
	// they lie. nullptr for a dead handle.
	const d3d12Buffer_t *			Use( bufferHandle_t buffer );
	// For a program with usesJoints and no joint buffer: JOINT_TAIL zero bytes.
	[[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS	ZeroJoints() const;
	// Stride 0, (0,0,0,1) as four floats: GL's current value of an attribute the layout lacks.
	[[nodiscard]] D3D12_VERTEX_BUFFER_VIEW	DefaultAttribs() const;
	void							DestroyAll();		// Shutdown, the GPU idle
	[[nodiscard]] uint64			Bytes( bufferUsage_t usage ) const { return bytes[static_cast<size_t>( usage )]; }
	[[nodiscard]] uint64			MirrorBytes() const { return mirrorBytes; }
	[[nodiscard]] int				Num() const { return table.Num(); }
	[[nodiscard]] d3d12BufferPlacement_t	FramePlacement() const { return framePlacement; }
	[[nodiscard]] uint64			InPlaceBytesThisFrame() const { return inPlaceBytes; }	// the dirty bytes of mapped buffers
	void							ResetFrameCounters() { inPlaceBytes = 0; }

private:
	const idD3D12Device &			device;
	idD3D12Graveyard &				graveyard;
	idD3D12Uploader &				uploader;
	idD3D12Barriers &				barriers;
	d3d12BufferPlacement_t			framePlacement;
	idD3D12HandleTable<d3d12Buffer_t, bufferHandle_t>	table;
	idComPtr<ID3D12Resource>		zeroJoints;
	idComPtr<ID3D12Resource>		defaultAttribs;
	std::array<uint64, 2>			bytes = {};		// GPU memory by bufferUsage_t
	uint64							mirrorBytes = 0;
	uint64							inPlaceBytes = 0;
};
