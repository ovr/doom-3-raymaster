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

/*
	idD3D12Context: the composition root of the Direct3D 12 backend. Only the facade
	(idRenderBackendD3D12) knows it. It holds every subsystem by value; members are built in the
	order below and destroyed in the reverse order after WaitIdle, so a member may keep references
	to the members declared before it. The context lives on the heap and never moves.

	idRenderBackendD3D12                     the facade; renderBackend points at it
	 └── std::unique_ptr<idD3D12Context>      Init .. Shutdown; implements idD3D12Submitter
	      ├── idD3D12Library                  d3d12.dll, dxgi.dll, d3dcompiler_47.dll from System32
	      ├── idD3D12Device                   IDXGIFactory6, IDXGIAdapter1, ID3D12Device2, info queues, features
	      ├── idD3D12LiveObjectReport         the teardown messages, Debug: what is alive once the members below are gone
	      ├── idD3D12Queue                    ID3D12CommandQueue, ID3D12Fence; Waited() = the last explicit wait
	      ├── idD3D12Graveyard                GPU objects that die after the fence of their last list
	      ├── idD3D12CommandStream            ID3D12GraphicsCommandList1 + allocators reused by fence
	      └── optional<idD3D12SwapChain>      IDXGISwapChain3, 3 back buffers + RTVs, latency object

	Sync points: every wait is explicit, and everything that frees or reuses memory reads
	idD3D12Queue::Waited(), never the fence, so two runs record the same commands.

	  point                          waits for                     why
	  Present                        the fence of the frame        the vertex cache remaps the set the frame read (R3, R8)
	  Resize, vid_restart, Shutdown  a signal after the last Present  ResizeBuffers and releasing a back buffer need an idle queue
*/

#include "d3d12_Device.h"
#include "d3d12_Output.h"

class idD3D12Context final : public idD3D12Submitter {
public:
	// Builds into a local object and returns it only complete: a failed Init leaves nothing behind.
	[[nodiscard]] static std::expected<std::unique_ptr<idD3D12Context>, idStr>	Create( const renderBackendInit_t & init );
									~idD3D12Context();	// WaitIdle (skipped when lost), CollectAll, the last messages
									idD3D12Context( const idD3D12Context & ) = delete;
	idD3D12Context &				operator=( const idD3D12Context & ) = delete;

	ID3D12GraphicsCommandList1 &	List() override;
	uint64							SubmitChunk() override;
	void							WaitFor( uint64 fence ) override;
	void							WaitIdle() override;
	uint64							PendingFence() const override { return queue.LastSignaled() + 1; }
	uint64							ListSerial() const override { return commands.Serial(); }
	uint64							WaitedFence() const override { return queue.Waited(); }

	void							BeginFrame( int width, int height );	// follows a window that changed size on its own
	void							EndFrame();								// submits; no wait
	[[nodiscard]] presentResult_t	Present( presentMode_t mode );			// clear, submit, present, wait, retire, latency
	void							Resize( int width, int height );		// between frames; 0 x 0 (minimized) is ignored
	void							PrintInfo() const;

	idD3D12Library					library;
	idD3D12Device					device;
	idD3D12LiveObjectReport			liveObjects;
	idD3D12Queue					queue;
	idD3D12Graveyard				graveyard;
	idD3D12CommandStream			commands;
	std::optional<idD3D12SwapChain>	swapChain;

private:
									idD3D12Context( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice );
	[[nodiscard]] std::expected<void, idStr>	CreateOutput( HWND window, int width, int height );
	void							Retire( uint64 waitedFence );	// graves up to an explicitly waited fence
};
