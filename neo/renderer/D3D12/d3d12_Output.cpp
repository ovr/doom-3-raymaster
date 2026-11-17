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

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Output.h"

// The engine owns full screen (a borderless window, win_window.cpp) and Alt+Enter (the wndproc), so
// DXGI is told to leave the window alone and SetFullscreenState is never called.
std::expected<idD3D12SwapChain, idStr> idD3D12SwapChain::Create( const idD3D12Device & owner, idD3D12Queue & queue,
	HWND window, int clientWidth, int clientHeight ) {
	idD3D12SwapChain created;
	created.device = &owner;
	created.width = Max( clientWidth, 1 );
	created.height = Max( clientHeight, 1 );
	created.flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	if ( owner.Features().tearing ) {
		created.flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	}

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = static_cast<UINT>( created.width );
	desc.Height = static_cast<UINT>( created.height );
	desc.Format = FORMAT;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = BUFFERS;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = created.flags;

	idComPtr<IDXGISwapChain1> swapChain1;
	HRESULT hr = owner.Factory().CreateSwapChainForHwnd( &queue.Get(), window, &desc, nullptr, nullptr, &swapChain1 );
	if ( FAILED( hr ) ) {
		return std::unexpected( idStr( va( "CreateSwapChainForHwnd (%d x %d) failed: %s", created.width, created.height, D3D12_ErrorText( hr ).c_str() ) ) );
	}
	if ( FAILED( hr = swapChain1.As( &created.swapChain ) ) ) {
		return std::unexpected( idStr( va( "IDXGISwapChain3 is missing: %s", D3D12_ErrorText( hr ).c_str() ) ) );
	}
	owner.Factory().MakeWindowAssociation( window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES );
	created.swapChain->SetMaximumFrameLatency( 1 );
	created.latency.reset( created.swapChain->GetFrameLatencyWaitableObject() );

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heapDesc.NumDescriptors = BUFFERS;
	if ( FAILED( hr = owner.Get().CreateDescriptorHeap( &heapDesc, IID_PPV_ARGS( &created.rtvHeap ) ) ) ) {
		return std::unexpected( idStr( va( "the RTV heap of the swap chain: %s", D3D12_ErrorText( hr ).c_str() ) ) );
	}
	created.rtvHeap->SetName( L"doom3 back buffer RTVs" );
	created.AcquireBuffers();
	return created;
}

void idD3D12SwapChain::AcquireBuffers() {
	const UINT increment = device->Get().GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for ( UINT i = 0; i < BUFFERS; i++ ) {
		backBuffer_t & buffer = buffers[i];
		device->Check( swapChain->GetBuffer( i, IID_PPV_ARGS( &buffer.resource ) ), "IDXGISwapChain::GetBuffer" );
		buffer.resource->SetName( L"doom3 back buffer" );
		buffer.state = D3D12_RESOURCE_STATE_PRESENT;
		buffer.rtv = rtv;
		device->Get().CreateRenderTargetView( buffer.resource.Get(), nullptr, rtv );
		rtv.ptr += increment;
	}
}

// DXGI has no adaptive vsync: ADAPTIVE presents like VSYNC (caps.adaptiveSync is false).
presentResult_t idD3D12SwapChain::Present( presentMode_t mode ) {
	const bool immediate = ( mode == presentMode_t::IMMEDIATE );
	const UINT presentFlags = ( immediate && ( flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING ) != 0 ) ? DXGI_PRESENT_ALLOW_TEARING : 0;
	const HRESULT hr = swapChain->Present( immediate ? 0 : 1, presentFlags );
	if ( hr == DXGI_STATUS_OCCLUDED ) {
		return presentResult_t::OK;		// minimized or covered: the frame still counts
	}
	return device->Check( hr, "IDXGISwapChain::Present" ) ? presentResult_t::OK : presentResult_t::DEVICE_LOST;
}

void idD3D12SwapChain::WaitForLatency() {
	if ( latency ) {
		WaitForSingleObjectEx( latency.get(), 1000, TRUE );
	}
}

void idD3D12SwapChain::Resize( int newWidth, int newHeight ) {
	for ( backBuffer_t & buffer : buffers ) {
		buffer.resource.Reset();	// ResizeBuffers fails while any reference to a back buffer exists
	}
	width = Max( newWidth, 1 );
	height = Max( newHeight, 1 );
	device->Check( swapChain->ResizeBuffers( BUFFERS, static_cast<UINT>( width ), static_cast<UINT>( height ), FORMAT, flags ), "IDXGISwapChain::ResizeBuffers" );
	AcquireBuffers();
}

idD3D12SwapChain::backBuffer_t & idD3D12SwapChain::Current() {
	return buffers[swapChain->GetCurrentBackBufferIndex()];
}
