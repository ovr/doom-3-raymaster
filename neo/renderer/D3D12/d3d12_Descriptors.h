// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include <unordered_map>

#include "d3d12_Device.h"
#include "d3d12_RingAllocator.h"

// Never shader visible: the permanent SRV, RTV or DSV of a resource. Slots are stable; a full pool adds a heap.
class idD3D12DescriptorPool {
public:
	static constexpr uint32			HEAP_SIZE = 4096;
									idD3D12DescriptorPool( const idD3D12Device & device, D3D12_DESCRIPTOR_HEAP_TYPE type );
	[[nodiscard]] d3d12Descriptor_t	Allocate();					// the lowest free slot
	void							Free( d3d12Descriptor_t slot );
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE	Cpu( d3d12Descriptor_t slot ) const;

private:
	const idD3D12Device &			device;
	D3D12_DESCRIPTOR_HEAP_TYPE		type;
	uint32							increment;
	std::vector<idComPtr<ID3D12DescriptorHeap>>	heaps;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>	starts;		// of each heap
	std::vector<uint32>				freeSlots;					// a min-heap
	uint32							next = 0;
};

// The one shader-visible CBV/SRV/UAV heap: a fence-tagged ring of 8-slot tables, bound once per list.
class idD3D12DescriptorRing {
public:
	static constexpr uint32			SIZE = 65536;
	explicit						idD3D12DescriptorRing( const idD3D12Device & device );
	[[nodiscard]] ID3D12DescriptorHeap *	Heap() const { return heap.Get(); }
	// Copies the sources into 8 fresh consecutive slots (one CopyDescriptors); nullopt when live tables leave no room.
	[[nodiscard]] std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>	TryCopyTable( std::span<const D3D12_CPU_DESCRIPTOR_HANDLE, MAX_TEXTURE_SLOTS> sources );
	void							CloseChunk( uint64 fence ) { ring.CloseChunk( fence ); }
	void							Retire( uint64 waitedFence ) { ring.Retire( waitedFence ); }
	[[nodiscard]] uint64			OldestFence() const { return ring.OldestFence(); }

private:
	const idD3D12Device &			device;
	idComPtr<ID3D12DescriptorHeap>	heap;
	D3D12_CPU_DESCRIPTOR_HANDLE		cpuStart = {};
	D3D12_GPU_DESCRIPTOR_HANDLE		gpuStart = {};
	uint32							increment;
	idD3D12RingAllocator			ring;
};

// The packed samplers (D3D12_PackSampler) of the 8 slots of one draw.
struct d3d12SamplerTableKey_t {
	std::array<uint64, MAX_TEXTURE_SLOTS>	samplers = {};
	constexpr bool					operator==( const d3d12SamplerTableKey_t & ) const = default;
};
struct d3d12SamplerTableHash_t {
	[[nodiscard]] size_t			operator()( const d3d12SamplerTableKey_t & key ) const;
};

// The one shader-visible sampler heap: 256 tables of 8, each built once for its key and kept while it is
// used. A table is rewritten only when its last use has been waited for: the oldest use first, then the
// lowest index, so two runs build the same tables.
class idD3D12SamplerHeap {
public:
	static constexpr uint32			TABLES = 256;
	explicit						idD3D12SamplerHeap( const idD3D12Device & device );
	[[nodiscard]] ID3D12DescriptorHeap *	Heap() const { return heap.Get(); }
	// The table of key, used by the list that signals useFence; nullopt when every table is used after waitedFence.
	[[nodiscard]] std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>	TryTable( const d3d12SamplerTableKey_t & key, uint64 useFence, uint64 waitedFence );
	[[nodiscard]] uint64			OldestUse() const;		// the fence to wait for when TryTable failed; 0 = nothing used
	[[nodiscard]] int				Used() const { return static_cast<int>( used ); }
	[[nodiscard]] int				BuiltThisFrame() const { return built; }
	void							ResetFrameCounters() { built = 0; }

private:
	struct table_t {
		d3d12SamplerTableKey_t		key;
		uint64						lastUse = 0;
	};
	void							Build( uint32 index, const d3d12SamplerTableKey_t & key );

	const idD3D12Device &			device;
	idComPtr<ID3D12DescriptorHeap>	heap;
	D3D12_CPU_DESCRIPTOR_HANDLE		cpuStart = {};
	D3D12_GPU_DESCRIPTOR_HANDLE		gpuStart = {};
	uint32							increment;
	std::unordered_map<d3d12SamplerTableKey_t, uint32, d3d12SamplerTableHash_t>	lookup;
	std::array<table_t, TABLES>		tables = {};
	uint32							used = 0;			// tables 0 .. used - 1 hold a key
	int								built = 0;
};
