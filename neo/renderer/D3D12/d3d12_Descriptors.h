// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include <unordered_map>

#include "d3d12_Device.h"

// The permanent SRV, RTV or DSV of a resource, in heaps that are never shader visible. Slots are stable; a
// full pool adds a heap. An SRV pool made with shaderVisible slots also keeps every view in the same slot of
// one shader-visible heap, the ResourceDescriptorHeap of every draw: a slot is the index bindless.hlsli reads.
class idD3D12DescriptorPool {
public:
	static constexpr uint32			HEAP_SIZE = 4096;
									idD3D12DescriptorPool( const idD3D12Device & device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 shaderVisible = 0 );
	[[nodiscard]] d3d12Descriptor_t	Allocate();					// the lowest free slot; past the shader-visible slots a fatal error
	void							Free( d3d12Descriptor_t slot );
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE	Cpu( d3d12Descriptor_t slot ) const;
	// The view in its slot and in the shader-visible heap. A slot a submitted list may still read is buried, never
	// written again: the GPU reads the heap when it runs the list.
	void							CreateSrv( d3d12Descriptor_t slot, ID3D12Resource & resource, const D3D12_SHADER_RESOURCE_VIEW_DESC & desc );
	[[nodiscard]] ID3D12DescriptorHeap *	ShaderVisibleHeap() const { return visible.Get(); }
	[[nodiscard]] int				Used() const { return static_cast<int>( next - freeSlots.size() ); }

private:
	const idD3D12Device &			device;
	D3D12_DESCRIPTOR_HEAP_TYPE		type;
	uint32							increment;
	std::vector<idComPtr<ID3D12DescriptorHeap>>	heaps;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>	starts;		// of each heap
	std::vector<uint32>				freeSlots;					// a min-heap
	uint32							next = 0;
	uint32							visibleSize = 0;
	idComPtr<ID3D12DescriptorHeap>	visible;
	D3D12_CPU_DESCRIPTOR_HANDLE		visibleStart = {};
};

// The one shader-visible sampler heap, the SamplerDescriptorHeap of every draw: a slot per packed sampler
// (D3D12_PackSampler) that something holds. A slot nobody holds goes through the graveyard and is written
// again only when no list can read it, so no list waits for a sampler.
class idD3D12SamplerHeap {
public:
	static constexpr uint32			SIZE = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
									idD3D12SamplerHeap( const idD3D12Device & device, idD3D12Graveyard & graveyard );
	[[nodiscard]] ID3D12DescriptorHeap *	Heap() const { return heap.Get(); }
	[[nodiscard]] uint32			Acquire( uint64 packed );	// a fatal error past SIZE samplers held at once
	void							Release( uint32 slot );		// once per Acquire
	[[nodiscard]] uint32			FallbackSlot() const { return fallback; }	// packed 0, held for the heap's life
	[[nodiscard]] int				Used() const { return static_cast<int>( lookup.size() ); }
	[[nodiscard]] int				BuiltThisFrame() const { return built; }
	void							ResetFrameCounters() { built = 0; }
	void							Free( uint32 slot ) { freeSlots.push_back( slot ); }	// the graveyard, once the GPU is past it

private:
	struct held_t {
		uint64						packed = 0;
		int							references = 0;
	};
	const idD3D12Device &			device;
	idD3D12Graveyard &				graveyard;
	idComPtr<ID3D12DescriptorHeap>	heap;
	D3D12_CPU_DESCRIPTOR_HANDLE		cpuStart = {};
	uint32							increment;
	std::unordered_map<uint64, uint32>	lookup;
	std::vector<held_t>				slots;
	std::vector<uint32>				freeSlots;
	uint32							fallback = 0;
	int								built = 0;
};
