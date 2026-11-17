/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Dmitry Patsura

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma once

#include "d3d12_Device.h"

// Flip-model swap chain on the engine's window: 3 buffers, frame latency 1, tearing only at interval 0, no Alt+Enter.
// Its three RTVs live in a heap of its own, as long as the swap chain.
class idD3D12SwapChain {
public:
	static constexpr UINT			BUFFERS = 3;
	static constexpr DXGI_FORMAT	FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

	struct backBuffer_t {
		idComPtr<ID3D12Resource>	resource;
		D3D12_RESOURCE_STATES		state = D3D12_RESOURCE_STATE_PRESENT;
		D3D12_CPU_DESCRIPTOR_HANDLE	rtv = {};
	};

	[[nodiscard]] static std::expected<idD3D12SwapChain, idStr>	Create( const idD3D12Device & owner, idD3D12Queue & queue,
										HWND window, int clientWidth, int clientHeight );
	[[nodiscard]] presentResult_t	Present( presentMode_t mode );		// OCCLUDED → OK; removed, reset, hung → DEVICE_LOST
	void							WaitForLatency();					// the waitable object, at most 1 s
	void							Resize( int newWidth, int newHeight );	// GPU idle; ResizeBuffers with the creation flags
	[[nodiscard]] backBuffer_t &	Current();
	[[nodiscard]] int				Width() const { return width; }
	[[nodiscard]] int				Height() const { return height; }

private:
									idD3D12SwapChain() = default;
	void							AcquireBuffers();

	const idD3D12Device *			device = nullptr;	// a pointer: the object is moved once, out of Create
	idComPtr<IDXGISwapChain3>		swapChain;
	idComPtr<ID3D12DescriptorHeap>	rtvHeap;
	std::array<backBuffer_t, BUFFERS>	buffers;
	idD3D12Event					latency;
	UINT							flags = 0;		// the same in CreateSwapChainForHwnd and ResizeBuffers
	int								width = 0;
	int								height = 0;
};
