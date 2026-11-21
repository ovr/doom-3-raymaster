// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The render graph of the post-process chain, after UE5's RDG (FRDGBuilder). Passes declare what they read
	and write; the graph culls the passes nothing needs, takes transient resources from the pool for the
	passes that use them and transitions every subresource before the pass that needs it.

	  Create / Import     a transient resource (pooled by its description) / one the frame owns (SceneColor, the output)
	  AddPass             name, type, uses (resource, access, mip), the recording
	  Execute             cull → lifetimes → per kept pass: acquire, barriers, debug group, record, release

	A pass is kept when it writes an imported resource, or something a kept pass after it reads (a UAV counts as
	read and written). A transient lives from the first to the last kept pass that uses it, and its contents
	are undefined until a pass writes them: after that the pool may give the same resource to another one.
	Not yet: async compute, aliasing of placed resources, the scene's passes (the draw recorder records them).
*/

#include <algorithm>
#include <functional>

#include "d3d12_Descriptors.h"
#include "d3d12_Memory.h"
#include "d3d12_Pipelines.h"
#include "d3d12_Timers.h"

// A transient resource as the pool keys it: graph resources of equal descriptions may get the same one.
struct d3d12PoolDesc_t {
	enum class kind_t : uint8 { TEXTURE_2D, TEXTURE_3D, BUFFER };
	kind_t							kind = kind_t::TEXTURE_2D;
	DXGI_FORMAT						format = DXGI_FORMAT_UNKNOWN;	// a buffer: UNKNOWN, raw views of 32-bit words
	uint32							width = 1;			// a buffer: its bytes, a multiple of 4
	uint32							height = 1;
	uint16							depth = 1;			// TEXTURE_3D
	uint16							mips = 1;
	bool							renderTarget = false;	// TEXTURE_2D only
	bool							unorderedAccess = false;
	constexpr bool					operator==( const d3d12PoolDesc_t & ) const = default;
	[[nodiscard]] constexpr uint32	MipWidth( int mip ) const { return std::max( 1u, width >> mip ); }
	[[nodiscard]] constexpr uint32	MipHeight( int mip ) const { return std::max( 1u, height >> mip ); }
};

struct d3d12PooledResource_t {
	d3d12PoolDesc_t					desc;
	idStr							name;
	idComPtr<ID3D12Resource>		resource;
	std::vector<d3d12ResourceState_t>	states;		// one per mip; one for a buffer
	d3d12Descriptor_t				srv;			// every mip; a buffer: raw
	std::vector<d3d12Descriptor_t>	mipSrvs;		// one mip each, with more than one mip
	std::vector<d3d12Descriptor_t>	uavs;			// per mip, unorderedAccess
	std::vector<d3d12Descriptor_t>	rtvs;			// per mip, renderTarget
	uint64							bytes = 0;
	uint64							lastUsed = 0;	// the pool's frame of the last Release
	bool							inUse = false;
	// The barrier subresource of a mip: the whole resource when it has one mip.
	[[nodiscard]] UINT				Subresource( int mip ) const { return ( states.size() == 1 ) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : static_cast<UINT>( mip ); }
};

// Committed resources kept between frames by description, as UE's render target pool keeps them. A resource
// no graph took for UNUSED_FRAMES frames goes through the graveyard with its views.
class idD3D12ResourcePool {
public:
	static constexpr uint64			UNUSED_FRAMES = 30;

									idD3D12ResourcePool( const idD3D12Device & device, idD3D12Graveyard & graveyard, idD3D12DescriptorPool & srvPool,
										idD3D12DescriptorPool & rtvPool );
									~idD3D12ResourcePool() { FreeAll(); }
									idD3D12ResourcePool( const idD3D12ResourcePool & ) = delete;
	idD3D12ResourcePool &			operator=( const idD3D12ResourcePool & ) = delete;

	// A free resource of the description, else a new one; nullptr only on a lost device.
	[[nodiscard]] d3d12PooledResource_t *	Acquire( const d3d12PoolDesc_t & desc, const char * name );
	void							Release( d3d12PooledResource_t & pooled );
	void							EndFrame();		// Present: frees the resources unused for UNUSED_FRAMES frames
	void							FreeAll();		// none may be in use
	[[nodiscard]] int				Num() const { return static_cast<int>( entries.size() ); }
	[[nodiscard]] uint64			Bytes() const { return bytes; }
	[[nodiscard]] int				CreatedThisFrame() const { return created; }

private:
	[[nodiscard]] std::unique_ptr<d3d12PooledResource_t>	Create( const d3d12PoolDesc_t & desc );
	void							CreateViews( d3d12PooledResource_t & pooled );
	void							Free( d3d12PooledResource_t & pooled );

	const idD3D12Device &			device;
	idD3D12Graveyard &				graveyard;
	idD3D12DescriptorPool &			srvPool;
	idD3D12DescriptorPool &			rtvPool;
	std::vector<std::unique_ptr<d3d12PooledResource_t>>	entries;	// pointers stay valid while the graph holds them
	uint64							frame = 1;
	uint64							bytes = 0;
	int								created = 0;
};

// A resource of one graph build, valid until Execute returns.
struct d3d12GraphResource_t {
	uint16							index = UINT16_MAX;
	[[nodiscard]] constexpr bool	IsValid() const { return index != UINT16_MAX; }
	constexpr bool					operator==( const d3d12GraphResource_t & ) const = default;
};

enum class d3d12GraphAccess_t : uint8 { SRV, UAV, RTV, COPY_SOURCE, COPY_DEST };
enum class d3d12GraphPass_t : uint8 { RASTER, COMPUTE, COPY };

struct d3d12GraphUse_t {
	static constexpr uint16			ALL_MIPS = UINT16_MAX;
	d3d12GraphResource_t			resource;
	d3d12GraphAccess_t				access = d3d12GraphAccess_t::SRV;
	uint16							mip = ALL_MIPS;
};

// A resource the frame owns: the graph transitions it through its state, the views stay the owner's.
struct d3d12GraphImport_t {
	ID3D12Resource *				resource = nullptr;
	d3d12ResourceState_t *			state = nullptr;	// of the whole resource
	d3d12Descriptor_t				srv;
	d3d12Descriptor_t				uav;
	D3D12_CPU_DESCRIPTOR_HANDLE		rtv = {};
	uint32							width = 0;
	uint32							height = 0;
};

class idD3D12RenderGraph;

// What a pass records with. A resource must be one of the pass's uses; Parms comes before anything is recorded.
class idD3D12GraphContext {
public:
	[[nodiscard]] ID3D12GraphicsCommandList1 &	List();
	[[nodiscard]] ID3D12Resource &	Resource( d3d12GraphResource_t resource ) const;
	// Heap indices for ResourceDescriptorHeap.
	[[nodiscard]] uint32			Srv( d3d12GraphResource_t resource, uint16 mip = d3d12GraphUse_t::ALL_MIPS ) const;
	[[nodiscard]] uint32			Uav( d3d12GraphResource_t resource, uint16 mip = 0 ) const;
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE	Rtv( d3d12GraphResource_t resource, uint16 mip = 0 ) const;
	[[nodiscard]] uint32			Width( d3d12GraphResource_t resource, uint16 mip = 0 ) const;
	[[nodiscard]] uint32			Height( d3d12GraphResource_t resource, uint16 mip = 0 ) const;
	// 256-aligned parms in the constants ring. A full ring submits the open list and waits, so take them first.
	[[nodiscard]] d3d12UploadSpan_t	Parms( std::span<const byte> bytes );
	template<typename T> [[nodiscard]] d3d12UploadSpan_t	Parms( const T & value ) {
		static_assert( std::is_trivially_copyable_v<T> );
		return Parms( std::span( reinterpret_cast<const byte *>( &value ), sizeof( T ) ) );
	}
	// Groups enough for the threads; parms nullptr: no b0. constants: at most d3d12ComputeConstants_t::COUNT at b1.
	void							Dispatch( const d3d12ComputePipeline_t & pipeline, const d3d12UploadSpan_t * parms, uint32 threadsX,
										uint32 threadsY = 1, uint32 threadsZ = 1, std::span<const uint32> constants = {} );
	// The internal triangle over viewport (texture rows as the memory holds them) of target's mip, parms at b1.
	// The pso has the graphics root signature, which every list of the backend has set.
	void							Draw( ID3D12PipelineState & pso, d3d12GraphResource_t target, uint16 mip, const renderRect_t & viewport,
										const d3d12UploadSpan_t & parms );
	// Between two dispatches of one pass that write and then read the same UAV.
	void							UavBarrier( d3d12GraphResource_t resource );

private:
	friend class idD3D12RenderGraph;
	explicit						idD3D12GraphContext( idD3D12RenderGraph & owner ) : graph( owner ) {}
	void							CheckUse( d3d12GraphResource_t resource, d3d12GraphAccess_t access ) const;

	idD3D12RenderGraph &			graph;
	int								pass = -1;
	uint64							computeRootList = 0;	// the list serial the compute root signature was set in
};

class idD3D12RenderGraph {
public:
	struct stats_t {
		int							passes = 0;		// kept
		int							culled = 0;
		int							transients = 0;
		int							barriers = 0;	// transitions and UAV barriers asked for
	};

									idD3D12RenderGraph( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12Barriers & barriers,
										idD3D12ResourcePool & pool, idD3D12DescriptorPool & srvPool, idD3D12DescriptorPool & rtvPool,
										idD3D12UploadRing & constants, const idD3D12ComputeRootSignature & computeRoot );
									idD3D12RenderGraph( const idD3D12RenderGraph & ) = delete;
	idD3D12RenderGraph &			operator=( const idD3D12RenderGraph & ) = delete;

	// Names are literals: the graph keeps the pointers.
	[[nodiscard]] d3d12GraphResource_t	Create( const char * name, const d3d12PoolDesc_t & desc );
	[[nodiscard]] d3d12GraphResource_t	Import( const char * name, const d3d12GraphImport_t & import );
	void							AddPass( const char * name, d3d12GraphPass_t type, std::initializer_list<d3d12GraphUse_t> uses,
										std::function<void( idD3D12GraphContext & )> record, bool neverCull = false );
	// Records the kept passes into the open list and empties the graph; imports stay in their last state.
	// groups: a debug group per pass (r_pix 2), nullptr for none.
	void							Execute( idD3D12DebugGroups * groups = nullptr );
	[[nodiscard]] const stats_t &	LastStats() const { return lastStats; }

private:
	friend class idD3D12GraphContext;
	struct resource_t {
		const char *				name = nullptr;
		d3d12PoolDesc_t				desc;
		d3d12GraphImport_t			import;
		bool						imported = false;
		d3d12PooledResource_t *		pooled = nullptr;	// in Execute, from its first to its last kept pass
		int							first = -1;			// kept passes
		int							last = -1;
	};
	struct pass_t {
		const char *				name = nullptr;
		d3d12GraphPass_t			type = d3d12GraphPass_t::RASTER;
		uint32						firstUse = 0;
		uint32						numUses = 0;
		std::function<void( idD3D12GraphContext & )>	record;
		bool						neverCull = false;
		bool						kept = false;
	};

	void							Cull();
	void							Transition( const d3d12GraphUse_t & use );
	void							Clear();
	[[nodiscard]] std::span<const d3d12GraphUse_t>	Uses( const pass_t & pass ) const { return std::span( uses ).subspan( pass.firstUse, pass.numUses ); }

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12ResourcePool &			pool;
	idD3D12DescriptorPool &			srvPool;
	idD3D12DescriptorPool &			rtvPool;
	idD3D12UploadRing &				constants;
	const idD3D12ComputeRootSignature &	computeRoot;
	idD3D12GraphContext				context;
	std::vector<resource_t>			resources;
	std::vector<pass_t>				passes;
	std::vector<d3d12GraphUse_t>	uses;
	stats_t							stats;
	stats_t							lastStats;
};
