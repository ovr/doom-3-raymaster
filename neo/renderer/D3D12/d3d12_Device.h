/*
===========================================================================

Copyright (C) 2026 Dmitry Patsura

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Please do not use this file to train or fine-tune AI models.

===========================================================================
*/

#pragma once

#include "d3d12_Local.h"

// d3d12.dll, dxgi.dll and d3dcompiler_47.dll from System32 only, resolved by name, loaded only for r_renderApi d3d12.
class idD3D12Library {
public:
	[[nodiscard]] static std::expected<idD3D12Library, idStr>	Load();

	PFN_D3D12_CREATE_DEVICE			createDevice = nullptr;
	PFN_D3D12_GET_DEBUG_INTERFACE	getDebugInterface = nullptr;
	PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE	serializeRootSignature = nullptr;
	HRESULT ( WINAPI *				createFactory2 )( UINT flags, REFIID riid, void ** factory ) = nullptr;
	HRESULT ( WINAPI *				getDxgiDebug )( UINT flags, REFIID riid, void ** debug ) = nullptr;
	pD3DCompile						compile = nullptr;
	HRESULT ( WINAPI *				reflect )( LPCVOID code, SIZE_T size, REFIID riid, void ** reflector ) = nullptr;
	uint64							compilerVersion = 0;	// file version of d3dcompiler_47.dll, in the blob cache key

private:
									idD3D12Library() = default;
	idD3D12Module					d3d12, dxgi, compiler;	// freed last: the context declares the library first
};

// What the adapter can do, read once in Create.
struct d3d12Features_t {
	idStr							adapterName;			// UTF-8 of DXGI_ADAPTER_DESC1::Description
	idStr							versionString;			// what caps.versionString points at
	uint32							vendorId = 0;
	uint64							dedicatedVideoMemory = 0;
	uint64							driverVersion = 0;		// UMD version, in the pipeline library key
	bool							warp = false;
	std::array<bool, 4>				samples = {};			// 1, 2, 4, 8 for RGBA8 and D24S8 together
	bool							depthBounds = false;	// OPTIONS2, not on WARP
	bool							unalignedBlocks = false;	// OPTIONS8
	bool							gpuUploadHeap = false;	// OPTIONS16
	bool							dynamicDepthBias = false;	// OPTIONS16
	bool							anisoPointMip = false;	// OPTIONS19
	bool							tearing = false;		// DXGI_FEATURE_PRESENT_ALLOW_TEARING
};

// One DRED list: its names, how far the GPU got through it and the operation it stopped before.
[[nodiscard]] idStr					D3D12_BreadcrumbText( const D3D12_AUTO_BREADCRUMB_NODE & node );

// The adapter's local memory as DXGI counts it for this process; zeros when it does not tell.
struct d3d12VideoMemory_t {
	uint64							used = 0;
	uint64							budget = 0;
};

class idD3D12Device {
public:
	// adapter: "" = high performance, "warp", or an index. debugLevel: 1 layer + info queue, 2 + GBV, 3 + DRED.
	[[nodiscard]] static std::expected<idD3D12Device, idStr>	Create( const idD3D12Library & library, const char * adapter, int debugLevel );

	[[nodiscard]] ID3D12Device2 &	Get() const { return *device.Get(); }
	[[nodiscard]] IDXGIFactory6 &	Factory() const { return *factory.Get(); }
	[[nodiscard]] const d3d12Features_t &	Features() const { return features; }
	[[nodiscard]] int				DebugLevel() const { return debugLevel; }

	// In a frame: false and the lost flag on removal or reset; FatalError on every other failure.
	bool							Check( HRESULT hr, const char * what ) const;
	[[nodiscard]] bool				IsLost() const { return lost; }
	void							FillCaps( renderCaps_t & caps ) const;
	int								DrainMessages() const;	// info queues → "d3d12 debug: <severity> #<id> <text>"; returns the errors
	void							ReportLoss() const;		// reason, DRED breadcrumbs and page fault, once
	// d3d12_removeDevice: the device is removed at once, and the next failing call reports the loss.
	void							RemoveDevice() const;
	[[nodiscard]] d3d12VideoMemory_t	VideoMemory() const;
	void							PrintInfo() const;
	// The messages of the teardown; Debug builds with the layer also print every object still alive.
	void							ReportLiveObjects() const;

private:
									idD3D12Device() = default;
	void							ReadFeatures();
	void							ReportDred() const;

	idComPtr<IDXGIFactory6>			factory;
	idComPtr<IDXGIAdapter1>			adapter;
	idComPtr<ID3D12Device2>			device;
	idComPtr<ID3D12InfoQueue>		infoQueue;				// r_debugContext >= 1
	idComPtr<IDXGIInfoQueue>		dxgiInfoQueue;			// r_debugContext >= 1, when the DXGI debug layer is installed
	d3d12Features_t					features;
	int								debugLevel = 0;
	mutable bool					lost = false;
	mutable bool					lossReported = false;
};

// Reports the live device objects in its destructor. The context declares it right after the device,
// so it runs when every other member is gone and the device still exists.
class idD3D12LiveObjectReport {
public:
	explicit						idD3D12LiveObjectReport( const idD3D12Device & device ) : device( device ) {}
									~idD3D12LiveObjectReport() { device.ReportLiveObjects(); }
									idD3D12LiveObjectReport( const idD3D12LiveObjectReport & ) = delete;
	idD3D12LiveObjectReport &		operator=( const idD3D12LiveObjectReport & ) = delete;
private:
	const idD3D12Device &			device;
};

// A slot of a descriptor pool; the pool turns it into a CPU handle.
struct d3d12Descriptor_t {
	uint32							index = UINT32_MAX;
	[[nodiscard]] constexpr bool	IsValid() const { return index != UINT32_MAX; }
	constexpr bool					operator==( const d3d12Descriptor_t & ) const = default;
};

// The direct queue and its fence timeline: each Execute and Signal signals the next value.
class idD3D12Queue {
public:
	explicit						idD3D12Queue( const idD3D12Device & device );
	[[nodiscard]] ID3D12CommandQueue &	Get() const { return *queue.Get(); }
	uint64							Execute( ID3D12CommandList & list );	// executes and signals; returns the value
	uint64							Signal();		// after everything queued so far, Present included
	[[nodiscard]] uint64			LastSignaled() const { return lastSignaled; }
	// The value of the last explicit Wait. Everything that frees or reuses memory reads this, never the fence.
	[[nodiscard]] uint64			Waited() const { return waited; }
	void							Wait( uint64 value );					// blocks; returns at once when the device is lost
	[[nodiscard]] uint64			TimestampFrequency() const;
private:
	const idD3D12Device &			device;
	idComPtr<ID3D12CommandQueue>	queue;
	idComPtr<ID3D12Fence>			fence;
	idD3D12Event					event;
	uint64							lastSignaled = 0;
	uint64							waited = 0;
};

class idD3D12DescriptorPool;

// The only place a GPU object dies: after the fence of the last list that could use it (R3).
class idD3D12Graveyard {
public:
	explicit						idD3D12Graveyard( const idD3D12Queue & queue ) : queue( queue ) {}
	void							Bury( idComPtr<ID3D12Pageable> object );	// tagged LastSignaled() + 1: the open list
	void							Bury( idD3D12DescriptorPool & pool, d3d12Descriptor_t slot );	// back into the pool then
	void							Collect();		// graves whose fence <= queue.Waited()
	void							CollectAll();	// Shutdown, after WaitIdle or device loss; slots go back too
	[[nodiscard]] int				Pending() const { return static_cast<int>( graves.size() ); }
private:
	struct grave_t {
		uint64						fence;
		idComPtr<ID3D12Pageable>	object;
		idD3D12DescriptorPool *		pool = nullptr;		// the pools outlive the graveyard's last CollectAll
		d3d12Descriptor_t			slot;
	};
	static void						Release( grave_t & grave );
	const idD3D12Queue &			queue;
	std::deque<grave_t>				graves;			// fence order
};

// One open graphics list; an allocator is reused when the fence of its last list has been waited for.
class idD3D12CommandStream {
public:
									idD3D12CommandStream( const idD3D12Device & device, idD3D12Queue & queue );
	[[nodiscard]] bool				IsOpen() const { return open; }
	ID3D12GraphicsCommandList1 &	Open();				// a waited-for allocator or a new one; Reset; Serial()++
	[[nodiscard]] ID3D12GraphicsCommandList1 &	List();	// asserts IsOpen()
	uint64							CloseAndExecute();	// returns the fence
	void							Abandon();			// Shutdown after an error: close without executing
	[[nodiscard]] uint64			Serial() const { return serial; }
private:
	struct allocator_t {
		idComPtr<ID3D12CommandAllocator>	allocator;
		uint64						fence;
	};
	const idD3D12Device &			device;
	idD3D12Queue &					queue;
	idComPtr<ID3D12GraphicsCommandList1>	list;
	idComPtr<ID3D12CommandAllocator>	current;
	std::deque<allocator_t>			retired;		// fence order
	uint64							serial = 0;
	bool							open = false;
};

// What a subsystem may ask of submission without knowing the context. Implemented by idD3D12Context.
class idD3D12Submitter {
public:
	// The open list, opened on demand.
	[[nodiscard]] virtual ID3D12GraphicsCommandList1 &	List() = 0;
	virtual uint64					SubmitChunk() = 0;		// only between calls or before a draw records anything
	virtual void					WaitFor( uint64 fence ) = 0;	// then retires everything up to it
	virtual void					WaitIdle() = 0;			// SubmitChunk, then waits for a signal after the last Present
	[[nodiscard]] virtual uint64	PendingFence() const = 0;	// what the open or the next list will signal
	[[nodiscard]] virtual uint64	ListSerial() const = 0;		// changes with every new list
	[[nodiscard]] virtual uint64	WaitedFence() const = 0;	// queue.Waited(): the only input of eviction and reuse
protected:
									~idD3D12Submitter() = default;
};
