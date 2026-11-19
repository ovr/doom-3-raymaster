// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Descriptors.h"
#include "d3d12_Memory.h"
#include "d3d12_StateMap.h"

// r_multiSamples → the scene's sample count: the largest of 2, 4, 8 at most requested that the adapter
// has for RGBA8 and D24S8 together (d3d12Features_t::samples, by log2); 1 below 2 or when none fits.
[[nodiscard]] UINT					D3D12_SceneSamples( int requested, std::span<const bool, 4> supported );

// The frame every draw, clear, copy and readback works on, in GL row order: memory row 0 is the bottom of
// the screen, so the rectangles of shared code are used as they are and only the present blit flips rows.
// Colour and depth are TYPELESS (UNORM + SRGB render target views, an R24 depth shader view); MSAA adds a resolve texture.
class idD3D12SceneTarget {
public:
	struct surface_t {
		idComPtr<ID3D12Resource>	resource;
		d3d12ResourceState_t		state;
		d3d12Descriptor_t			srv;			// colour: UNORM; depth: R24_UNORM_X8_TYPELESS
	};

									idD3D12SceneTarget( const idD3D12Device & device, idD3D12Graveyard & graveyard, idD3D12DescriptorPool & srvPool,
										idD3D12DescriptorPool & rtvPool, idD3D12DescriptorPool & dsvPool );
	void							Create( int width, int height, UINT samples );	// Init and Resize; the old resources are buried
	[[nodiscard]] d3d12TargetId_t	Target( bool srgb ) const { return d3d12TargetId_t::Of( srgb, samples ); }
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE	Rtv( bool srgb ) const { return rtvPool.Cpu( srgb ? rtvSrgb : rtvUnorm ); }
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE	Dsv() const { return dsvPool.Cpu( dsv ); }
	[[nodiscard]] surface_t &		Color() { return color; }
	[[nodiscard]] surface_t &		Depth() { return depth; }
	// One sample: Color(). MSAA: the colour resolved into the resolve texture first, only inside region when
	// there is one (a copy's box), which must lie inside the scene.
	[[nodiscard]] surface_t &		Resolved( idD3D12Barriers & barriers, idD3D12Submitter & submitter,
										std::optional<D3D12_RECT> region = std::nullopt );
	[[nodiscard]] int				Width() const { return width; }
	[[nodiscard]] int				Height() const { return height; }
	[[nodiscard]] UINT				Samples() const { return samples; }

private:
	void							CreateColor( surface_t & surface, UINT sampleCount, bool renderTarget, const wchar_t * name );
	void							CreateDepth();

	const idD3D12Device &			device;
	idD3D12Graveyard &				graveyard;
	idD3D12DescriptorPool &			srvPool;
	idD3D12DescriptorPool &			rtvPool;
	idD3D12DescriptorPool &			dsvPool;
	surface_t						color, depth, resolved;
	d3d12Descriptor_t				rtvUnorm, rtvSrgb, dsv;	// stable: Create rewrites the views
	int								width = 0;
	int								height = 0;
	UINT							samples = 1;
};

// Flip-model swap chain on the engine's window: 3 buffers, frame latency 1, tearing only at interval 0, no Alt+Enter.
class idD3D12SwapChain {
public:
	static constexpr UINT			BUFFERS = 3;
	static constexpr DXGI_FORMAT	FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

	struct backBuffer_t {
		idComPtr<ID3D12Resource>	resource;
		d3d12ResourceState_t		state;			// COMMON is PRESENT
		d3d12Descriptor_t			rtv;
	};

	[[nodiscard]] static std::expected<idD3D12SwapChain, idStr>	Create( const idD3D12Device & owner, idD3D12Queue & queue,
										idD3D12DescriptorPool & rtvs, HWND window, int clientWidth, int clientHeight );
	[[nodiscard]] presentResult_t	Present( presentMode_t mode );		// OCCLUDED → OK; removed, reset, hung → DEVICE_LOST
	void							WaitForLatency();					// the waitable object, at most 1 s
	void							Resize( int newWidth, int newHeight );	// GPU idle; ResizeBuffers with the creation flags
	[[nodiscard]] backBuffer_t &	Current();
	[[nodiscard]] int				Width() const { return width; }
	[[nodiscard]] int				Height() const { return height; }

private:
									idD3D12SwapChain() = default;
	void							AcquireBuffers();

	const idD3D12Device *			device = nullptr;	// pointers: the object is moved once, out of Create
	idD3D12DescriptorPool *			rtvPool = nullptr;
	idComPtr<IDXGISwapChain3>		swapChain;
	std::array<backBuffer_t, BUFFERS>	buffers;
	idD3D12Event					latency;
	UINT							flags = 0;		// the same in CreateSwapChainForHwnd and ResizeBuffers
	int								width = 0;
	int								height = 0;
};

// A copy into a readback buffer with 256-byte rows, WaitIdle, RGBA → tight RGB, rows in memory order (GL's bottom-up).
class idD3D12Readback {
public:
									idD3D12Readback( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12Barriers & barriers,
										idD3D12Graveyard & graveyard );
	// x, y in GL's output coordinates. flipRows: the source is a back buffer, whose row 0 is the top of the screen.
	void							Read( ID3D12Resource & source, d3d12ResourceState_t & state, int x, int y, int width, int height,
										byte * rgb, bool flipRows );

private:
	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12Graveyard &				graveyard;
	idComPtr<ID3D12Resource>		buffer;			// grows; the old one is buried
	uint64							size = 0;
};
