// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include <map>
#include <set>
#include <string>

#include "d3d12_Programs.h"
#include "d3d12_StateMap.h"

/*
	The one root signature of every program and every internal pass, version 1.1, 32 DWORDs, with both heaps
	directly indexed (SM 6.6 ResourceDescriptorHeap and SamplerDescriptorHeap):

	  0  VERTEX_PARMS    root CBV b0, vertex shader     the program's vertex parms (GL's _va_[])
	  1  FRAGMENT_PARMS  root CBV b1, pixel shader      the program's fragment parms (GL's _fa_[])
	  2  JOINTS          root CBV b2, vertex shader     the joint block at the draw's joint offset
	  3  DRAW_RESOURCES  26 constants b3, all stages    d3d12DrawResources_t: the heap indices of the slots'
	                                                    views and samplers (base/shaders/include/bindless.hlsli),
	                                                    colorFlags and preExposure (include/colorspace.hlsli)
*/
class idD3D12RootSignature {
public:
									idD3D12RootSignature( const idD3D12Device & device, const idD3D12Library & library );
	[[nodiscard]] ID3D12RootSignature *	Get() const { return signature.Get(); }

private:
	idComPtr<ID3D12RootSignature>	signature;
};

/*
	The root signature of the backend's own compute passes (render graph passes), both heaps directly indexed:

	  0  PARMS      root CBV b0                   the pass's parms, heap indices of its views included
	  1  CONSTANTS  8 constants b1                what changes between the dispatches of a pass (a level)
*/
class idD3D12ComputeRootSignature {
public:
									idD3D12ComputeRootSignature( const idD3D12Device & device, const idD3D12Library & library );
	[[nodiscard]] ID3D12RootSignature *	Get() const { return signature.Get(); }

private:
	idComPtr<ID3D12RootSignature>	signature;
};

// A compute PSO and the numthreads of its stage, which turns threads into groups.
struct d3d12ComputePipeline_t {
	idComPtr<ID3D12PipelineState>	pso;
	std::array<uint32, 3>			threadGroup = {};
	[[nodiscard]] explicit			operator bool() const { return pso != nullptr; }
};
// nullptr pso only on a lost device.
[[nodiscard]] d3d12ComputePipeline_t	D3D12_CreateComputePipeline( const idD3D12Device & device, const idD3D12ComputeRootSignature & root,
											const d3d12Bytecode_t & code, const d3d12StageReflection_t & reflection, const wchar_t * name );

// pipelineKey_t::depthBias → a D3D12 bias. Each slot holds a distinct bias and none holds no bias, so
// equal slots ⇔ equal PSOs; slots live as long as the backend.
class idD3D12DepthBiasTable {
public:
	// 0 for no bias. A full table warns once and gives the nearest bias it has, no bias included.
	[[nodiscard]] uint8				Slot( float factor, float units );
	// The same for a bias the journal kept; it counts no draw.
	[[nodiscard]] uint8				SlotOf( const d3d12DepthBias_t & bias ) { bool nearestOne = false; return Find( bias, nearestOne ); }
	[[nodiscard]] d3d12DepthBias_t	operator[]( uint8 slot ) const;		// slot 0: none
	[[nodiscard]] int				Num() const { return count; }
	[[nodiscard]] int				NearestThisFrame() const { return nearest; }	// draws whose own bias did not fit
	void							ResetFrameCounters() { nearest = 0; }

private:
	[[nodiscard]] uint8				Find( const d3d12DepthBias_t & wanted, bool & nearestOne );	// a new bias takes a free slot
	[[nodiscard]] uint8				NearestSlot( const d3d12DepthBias_t & wanted ) const;

	std::array<d3d12DepthBias_t, 255>	biases = {};	// slot - 1
	int								count = 0;
	float							lastFactor = 0.0f;	// shared code sets one pair for many draws
	float							lastUnits = 0.0f;
	uint8							lastSlot = 0;
	bool							lastNearest = false;
	bool							reportedFull = false;
	int								nearest = 0;
};

// A key as the journal keeps it. Bias slots are numbered in the order of first use, which differs from run to
// run, so the entry holds the bias itself.
struct d3d12JournalEntry_t {
	pipelineKey_t					key = {};		// normalized; program and depthBias 0
	d3d12DepthBias_t				bias;
	bool							operator==( const d3d12JournalEntry_t & ) const = default;
};

// Every key the game drew with, by the program's journal name ("<vs>|<ps>"), which unlike a handle is the same in
// every run. The context reads it at Init and writes it after each level load and at Shutdown; CreateProgram warms
// its keys. It only grows: a key of another sample count waits for the run that has it.
class idD3D12PipelineJournal {
public:
	static constexpr uint32			VERSION = 2;	// bump when the bits of pipelineKey_t change their meaning

	void							Record( std::string_view journalName, const d3d12JournalEntry_t & entry );	// a known one is ignored
	[[nodiscard]] std::span<const d3d12JournalEntry_t>	EntriesFor( std::string_view journalName ) const;
	// false: not a journal of this VERSION (none yet, stale or cut short), and the journal stays empty.
	bool							Read( std::span<const byte> file );
	[[nodiscard]] std::vector<byte>	Write() const;
	[[nodiscard]] bool				Dirty() const { return dirty; }
	void							MarkSaved() { dirty = false; }
	[[nodiscard]] int				NumPrograms() const { return static_cast<int>( entries.size() ); }
	[[nodiscard]] int				NumEntries() const { return numEntries; }

private:
	std::map<std::string, std::vector<d3d12JournalEntry_t>, std::less<>>	entries;	// sorted, so equal journals write equal files
	int								numEntries = 0;
	bool							dirty = false;
};

// The name of a PSO in the library: an MD5 of everything its stream is built from, so a name never meets
// another description.
struct d3d12PipelineName_t {
	std::array<byte, 16>			digest = {};
	[[nodiscard]] std::array<wchar_t, 40>	Text() const;	// "doom3 <32 hex digits>"
	auto							operator<=>( const d3d12PipelineName_t & ) const = default;
};
[[nodiscard]] d3d12PipelineName_t	D3D12_PipelineLibraryName( const d3d12Program_t & program, const d3d12JournalEntry_t & entry, bool depthBounds );

// ID3D12PipelineLibrary1 over the bytes of a file: a header with the driver version, the names stored, then
// what the library serialized. Load asks the library only for a name it has: the debug layer warns about any
// other.
class idD3D12PipelineLibrary {
public:
	static constexpr uint32			VERSION = 3;	// bump when the root signature or the pipeline stream changes

	// nullopt: the adapter keeps no pipeline library. A file of another driver, adapter or VERSION, or a corrupt
	// one, gives an empty library, whose Write then replaces the file.
	[[nodiscard]] static std::optional<idD3D12PipelineLibrary>	Open( const idD3D12Device & device, std::vector<byte> file );
	// Moved only: the blob's bytes move with it. A copy would share the library over the old blob, and a move
	// assignment would free the old blob before the old library.
									idD3D12PipelineLibrary( idD3D12PipelineLibrary && ) = default;
									idD3D12PipelineLibrary( const idD3D12PipelineLibrary & ) = delete;
	idD3D12PipelineLibrary &		operator=( const idD3D12PipelineLibrary & ) = delete;
	idD3D12PipelineLibrary &		operator=( idD3D12PipelineLibrary && ) = delete;
	// nullptr: not stored yet.
	[[nodiscard]] idComPtr<ID3D12PipelineState>	Load( const d3d12PipelineName_t & name, const D3D12_PIPELINE_STATE_STREAM_DESC & desc ) const;
	void							Store( const d3d12PipelineName_t & name, ID3D12PipelineState & pso );
	[[nodiscard]] std::vector<byte>	Write() const;	// empty when the library cannot serialize
	[[nodiscard]] bool				Dirty() const { return dirty; }
	void							MarkSaved() { dirty = false; }
	[[nodiscard]] bool				FromFile() const { return fromFile; }	// opened over a file of this driver
	[[nodiscard]] uint64			FileBytes() const { return static_cast<uint64>( blob.size() ); }
	[[nodiscard]] int				Num() const { return static_cast<int>( names.size() ); }

private:
	explicit						idD3D12PipelineLibrary( const idD3D12Device & owner ) : device( &owner ) {}

	std::vector<byte>				blob;			// declared first: D3D12 reads it in place, so it outlives library
	idComPtr<ID3D12PipelineLibrary1>	library;
	std::set<d3d12PipelineName_t>	names;			// sorted: equal libraries write equal indexes
	const idD3D12Device *			device;			// a pointer: the object is moved out of Open
	bool							dirty = false;
	bool							fromFile = false;
	bool							reportedStore = false;
};

// The PSOs of every program by normalized key; they die with their program. A key is created on first use and
// recorded in the journal, which the next run's CreateProgram replays, so a frame of that run creates none.
class idD3D12PipelineFactory {
public:
	// What made the PSOs of the keys a program asked for, since Init or since the last TakeLevelStats.
	struct stats_t {
		int							drawn = 0;		// new keys of draws, the PSOs a frame waited for
		int							warmed = 0;		// journal keys made in CreateProgram
		int							loaded = 0;		// of both, found in the library
		uint64						drawnMicroseconds = 0;
		uint64						warmMicroseconds = 0;
	};

	// journal or library nullptr: none (the tests; r_d3d12PipelineLibrary 0 or an adapter without one).
									idD3D12PipelineFactory( const idD3D12Device & device, const idD3D12RootSignature & root,
										idD3D12PipelineJournal * journal, idD3D12PipelineLibrary * library );
	// nullptr only on a lost device.
	[[nodiscard]] ID3D12PipelineState *	Get( d3d12Program_t & program, const pipelineKey_t & key );
	// CreateProgram: the PSOs of the program's journal keys whose target has the scene's sample count.
	void							Warm( d3d12Program_t & program, UINT samples );
	[[nodiscard]] idD3D12DepthBiasTable &	Bias() { return bias; }
	[[nodiscard]] const idD3D12DepthBiasTable &	Bias() const { return bias; }
	[[nodiscard]] int				CreatedThisFrame() const { return created; }	// by draws
	[[nodiscard]] int				Num() const { return total; }
	[[nodiscard]] const stats_t &	TotalStats() const { return totalStats; }
	[[nodiscard]] stats_t			TakeLevelStats();
	[[nodiscard]] bool				HasLibrary() const { return library != nullptr; }
	void							ResetFrameCounters() { created = 0; bias.ResetFrameCounters(); }

private:
	// The library's PSO, else a new one, stored in the library.
	[[nodiscard]] idComPtr<ID3D12PipelineState>	Make( const d3d12Program_t & program, const pipelineKey_t & key, bool & fromLibrary );
	void							Count( bool warm, bool fromLibrary, uint64 microseconds );

	const idD3D12Device &			device;
	const idD3D12RootSignature &	root;
	idD3D12PipelineJournal *		journal;
	idD3D12PipelineLibrary *		library;
	idD3D12DepthBiasTable			bias;
	stats_t							levelStats;
	stats_t							totalStats;
	int								created = 0;
	int								total = 0;		// created since Init; DestroyProgram does not count down
};
