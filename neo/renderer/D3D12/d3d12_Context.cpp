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
#include "d3d12_Context.h"

idCVar r_d3d12Adapter( "r_d3d12Adapter", "", CVAR_RENDERER | CVAR_INIT,
	"Direct3D 12 adapter: empty = the high-performance GPU, warp = the software reference, N = adapter N by GPU preference" );

namespace {

// The window colour until frames reach the back buffer.
constexpr float PRESENT_CLEAR_COLOR[4] = { 0.0f, 0.25f, 0.3f, 1.0f };

void Transition( ID3D12GraphicsCommandList1 & list, idD3D12SwapChain::backBuffer_t & buffer, D3D12_RESOURCE_STATES after ) {
	if ( buffer.state == after ) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = buffer.resource.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = buffer.state;
	barrier.Transition.StateAfter = after;
	list.ResourceBarrier( 1, &barrier );
	buffer.state = after;
}

}	// namespace

std::expected<std::unique_ptr<idD3D12Context>, idStr> idD3D12Context::Create( const renderBackendInit_t & init ) {
	auto loaded = idD3D12Library::Load();
	if ( !loaded ) {
		return std::unexpected( loaded.error() );
	}
	auto created = idD3D12Device::Create( *loaded, r_d3d12Adapter.GetString(), r_debugContext.GetInteger() );
	if ( !created ) {
		return std::unexpected( created.error() );
	}
	std::unique_ptr<idD3D12Context> context( new idD3D12Context( std::move( *loaded ), std::move( *created ) ) );
	if ( auto output = context->CreateOutput( static_cast<HWND>( init.nativeWindow ), init.width, init.height ); !output ) {
		return std::unexpected( output.error() );
	}
	context->device.DrainMessages();
	return context;
}

idD3D12Context::idD3D12Context( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice ) :
	library( std::move( loadedLibrary ) ),
	device( std::move( createdDevice ) ),
	liveObjects( device ),
	queue( device ),
	graveyard( queue ),
	commands( device, queue ) {
}

// Safe after a mid-frame error: the open list is closed without running. The members then die in
// reverse order, the library last.
idD3D12Context::~idD3D12Context() {
	commands.Abandon();
	if ( !device.IsLost() ) {
		WaitIdle();
	}
	graveyard.CollectAll();
	device.DrainMessages();
}

std::expected<void, idStr> idD3D12Context::CreateOutput( HWND window, int width, int height ) {
	auto created = idD3D12SwapChain::Create( device, queue, window, width, height );
	if ( !created ) {
		return std::unexpected( created.error() );
	}
	swapChain.emplace( std::move( *created ) );
	return {};
}

ID3D12GraphicsCommandList1 & idD3D12Context::List() {
	if ( !commands.IsOpen() ) {
		return commands.Open();
	}
	return commands.List();
}

uint64 idD3D12Context::SubmitChunk() {
	if ( !commands.IsOpen() ) {
		return queue.LastSignaled();
	}
	return commands.CloseAndExecute();
}

void idD3D12Context::WaitFor( uint64 fence ) {
	queue.Wait( fence );
	Retire( queue.Waited() );
}

// The fence of the last list comes before the Present that still reads the back buffer.
void idD3D12Context::WaitIdle() {
	SubmitChunk();
	WaitFor( queue.Signal() );
}

void idD3D12Context::Retire( [[maybe_unused]] uint64 waitedFence ) {
	graveyard.Collect();
}

// Dragging the border resizes the window without ResizeOutput; nothing is recorded yet, so the
// output can follow here.
void idD3D12Context::BeginFrame( int width, int height ) {
	if ( swapChain && ( width != swapChain->Width() || height != swapChain->Height() ) ) {
		Resize( width, height );
	}
}

void idD3D12Context::EndFrame() {
	SubmitChunk();
}

// Blocks until the GPU has finished the frame: the next frame maps the vertex cache set this one read.
presentResult_t idD3D12Context::Present( presentMode_t mode ) {
	if ( device.IsLost() || !swapChain ) {
		return presentResult_t::DEVICE_LOST;
	}
	idD3D12SwapChain::backBuffer_t & back = swapChain->Current();
	ID3D12GraphicsCommandList1 & list = List();
	Transition( list, back, D3D12_RESOURCE_STATE_RENDER_TARGET );
	list.ClearRenderTargetView( back.rtv, PRESENT_CLEAR_COLOR, 0, nullptr );
	Transition( list, back, D3D12_RESOURCE_STATE_PRESENT );
	const uint64 fence = SubmitChunk();

	const presentResult_t result = swapChain->Present( mode );
	WaitFor( fence );
	swapChain->WaitForLatency();
	return device.IsLost() ? presentResult_t::DEVICE_LOST : result;
}

void idD3D12Context::Resize( int width, int height ) {
	if ( width <= 0 || height <= 0 || !swapChain ) {
		return;
	}
	if ( width == swapChain->Width() && height == swapChain->Height() ) {
		return;
	}
	WaitIdle();
	swapChain->Resize( width, height );
	common->DPrintf( "d3d12: swap chain resized to %d x %d\n", width, height );
}

void idD3D12Context::PrintInfo() const {
	device.PrintInfo();
	const uint64 version = library.compilerVersion;
	common->Printf( "D3D12 shader compiler: d3dcompiler_47.dll %u.%u.%u.%u\n", static_cast<unsigned>( version >> 48 ),
		static_cast<unsigned>( ( version >> 32 ) & 0xFFFF ), static_cast<unsigned>( ( version >> 16 ) & 0xFFFF ), static_cast<unsigned>( version & 0xFFFF ) );
	if ( swapChain ) {
		common->Printf( "D3D12 swap chain: %d x %d, %u flip-discard buffers\n", swapChain->Width(), swapChain->Height(), idD3D12SwapChain::BUFFERS );
	}
}
