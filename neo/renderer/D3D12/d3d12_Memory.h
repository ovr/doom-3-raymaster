// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Device.h"
#include "d3d12_RingAllocator.h"

// A committed buffer of one heap type, named for the debug tools; nullptr only when the device is lost.
[[nodiscard]] idComPtr<ID3D12Resource>	D3D12_CreateBuffer( const idD3D12Device & device, D3D12_HEAP_TYPE heap, uint64 bytes,
											D3D12_RESOURCE_STATES state, const wchar_t * name );

// CPU-written memory the GPU reads once; write-combined: write it sequentially, never read it.
struct d3d12UploadSpan_t {
	byte *							cpu;
	D3D12_GPU_VIRTUAL_ADDRESS		gpu;
	ID3D12Resource *				buffer;
	uint64							offset;			// of cpu inside buffer
};

// A persistently mapped UPLOAD buffer used as a fence-tagged ring. It never submits: a full ring is the caller's problem.
class idD3D12UploadRing {
public:
									idD3D12UploadRing( const idD3D12Device & device, uint64 size, const wchar_t * name );
	[[nodiscard]] std::optional<d3d12UploadSpan_t>	TryAllocate( uint64 bytes, uint64 align );
	void							CloseChunk( uint64 fence ) { ring.CloseChunk( fence ); }
	void							Retire( uint64 waitedFence ) { ring.Retire( waitedFence ); }
	[[nodiscard]] uint64			OldestFence() const { return ring.OldestFence(); }
	[[nodiscard]] uint64			Size() const { return ring.Size(); }
	[[nodiscard]] uint64			Used() const { return ring.Used(); }

private:
	idComPtr<ID3D12Resource>		buffer;
	byte *							mapped = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS		gpuBase = 0;
	idD3D12RingAllocator			ring;
};

// The state of one tracked resource, kept in its owner's record from creation on.
struct d3d12ResourceState_t {
	D3D12_RESOURCE_STATES			current = D3D12_RESOURCE_STATE_COMMON;	// what it is in once the batch is flushed
	uint64							listSerial = 0;	// decaying resources: the list in which current holds
	int16							pending = -1;	// this resource's entry in the batch
	bool							decays = false;	// buffers: the runtime puts them back in COMMON after every list
};

// Legacy transitions batched until the next draw, clear, copy, resolve or submit; a reversed pending pair cancels out.
class idD3D12Barriers {
public:
	explicit						idD3D12Barriers( idD3D12Submitter & owner ) : submitter( owner ) {}
	void							Transition( ID3D12Resource & resource, d3d12ResourceState_t & state, D3D12_RESOURCE_STATES after );
	void							Flush();			// into submitter.List(); a no-op when empty
	void							Discard();			// the list was abandoned
	[[nodiscard]] int				IssuedThisFrame() const { return issued; }
	void							ResetFrameCounters() { issued = 0; }

private:
	static constexpr int			BATCH = 64;		// a full batch flushes itself
	void							Remove( int index );

	idD3D12Submitter &				submitter;
	std::array<D3D12_RESOURCE_BARRIER, BATCH>	batch = {};
	std::array<d3d12ResourceState_t *, BATCH>	owners = {};	// owners[i] is the state of batch[i]
	int								count = 0;
	int								issued = 0;		// flushed into lists
};

// CPU data → GPU resources through "staging", recorded in the open list, so GPU order gives R4: draws recorded
// before a copy read the old bytes. It runs between the calls of the contract, so it may submit and wait.
class idD3D12Uploader {
public:
	static constexpr uint64			PIECE = 16 << 20;	// the most one copy takes from the ring, and the submit pace

									idD3D12Uploader( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12UploadRing & staging,
										idD3D12Barriers & barriers, idD3D12Graveyard & graveyard );
	// Memory for one copy source: ring room up to PIECE, else a temporary UPLOAD buffer that dies with the open
	// list. Record the copy before anything submits. nullopt only on a lost device.
	[[nodiscard]] std::optional<d3d12UploadSpan_t>	Stage( uint64 bytes, uint64 align );
	// bytes → buffer at offset, in copies of at most PIECE; leaves the buffer in useState.
	void							BufferRange( ID3D12Resource & buffer, d3d12ResourceState_t & state, D3D12_RESOURCE_STATES useState,
										uint64 offset, std::span<const byte> bytes );
	// The rows a Stage( RowPitch * Height, 512 ) span holds → the region of one subresource at (0, 0); the
	// texture stays in COPY_DEST. Call it right after filling the span, before anything submits.
	void							TextureRegion( ID3D12Resource & texture, d3d12ResourceState_t & state, UINT subresource,
										const d3d12UploadSpan_t & span, const D3D12_SUBRESOURCE_FOOTPRINT & footprint, UINT rows );
	[[nodiscard]] uint64			BytesThisFrame() const { return bytesThisFrame; }
	void							ResetFrameCounters() { bytesThisFrame = 0; }

private:
	[[nodiscard]] d3d12UploadSpan_t	Room( uint64 bytes, uint64 align );	// a full ring: SubmitChunk, WaitFor( oldest chunk ), again
	void							Pace( uint64 bytes );	// submits after every PIECE copied into one list: the GPU copies while a level loads

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12UploadRing &				staging;
	idD3D12Barriers &				barriers;
	idD3D12Graveyard &				graveyard;
	uint64							paceList = 0;		// the list bytesInList counts for
	uint64							bytesInList = 0;
	uint64							bytesThisFrame = 0;
};
