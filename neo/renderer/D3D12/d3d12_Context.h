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
	      ├── idD3D12DebugGroups              r_pix 2: the group stack, ended at a submit and begun again in the next list
	      ├── idD3D12GpuTimers                r_pix 1 and the frame's GPU time: a TIMESTAMP query heap resolved in EndFrame
	      ├── idD3D12Barriers                 transitions batched until the next draw, clear, copy or submit
	      ├── idD3D12DescriptorPool x 3       SRV, RTV, DSV: the permanent views, never shader visible
	      ├── idD3D12DescriptorRing           the shader-visible CBV/SRV heap, 8-slot tables reused by fence
	      ├── idD3D12SamplerHeap              the shader-visible sampler heap, 256 tables of 8 kept by key
	      ├── idD3D12UploadRing               "constants": 16 MB UPLOAD for the root CBVs, reused by fence
	      ├── idD3D12UploadRing               "staging": 64 MB UPLOAD, the copy sources of buffers and textures
	      ├── idD3D12Uploader                 copies through "staging" into the open list, a submit every 16 MB
	      ├── idD3D12BufferStore              bufferHandle_t → FRAME in UPLOAD heaps, STATIC in DEFAULT + a CPU mirror
	      ├── idD3D12TextureStore             textureHandle_t → a committed texture + SRV, the black fallbacks
	      ├── idD3D12ShaderCompiler           FXC with constant flags, the DXBC cache in renderprogs/dxbc
	      ├── idD3D12ShaderLibrary            shaderHandle_t, programHandle_t → emitted stages, DXBC pairs, their PSOs
	      ├── idD3D12RootSignature            the 5 parameters of every PSO
	      ├── idD3D12PipelineFactory          normalized pipelineKey_t → a PSO kept with its program; the depth bias slots
	      ├── idD3D12SceneTarget              colour + depth in GL row order, the MSAA resolve texture
	      ├── optional<idD3D12SwapChain>      IDXGISwapChain3, 3 back buffers + RTVs, latency object
	      ├── idD3D12InternalPasses           the present blit with the gamma LUT, the masked clear, the depth copy, r_d3d12ShowImage
	      ├── idD3D12Readback                 copies into a READBACK buffer for ReadOutputPixels
	      ├── idD3D12ScreenCopies             CopyOutputToTexture, CopyDepthToTexture: scene boxes into textures
	      └── idD3D12DrawRecorder             clears and draws into the open pass: Prepare, Reserve, Record

	Sync points: every wait is explicit, and everything that frees or reuses memory reads
	idD3D12Queue::Waited(), never the fence, so two runs record the same commands.

	  point                          waits for                     why
	  Present                        the fence of the frame        the vertex cache remaps the set the frame read (R3, R8)
	  ReadOutputPixels               a signal after the copy       the CPU reads the readback buffer
	  a full ring                    the fence of its oldest chunk  that room is taken again
	  Resize, Shutdown               a signal after the last Present  ResizeBuffers and releasing a back buffer need an idle queue

	vid_restart only changes the window (R_SetNewMode( false )) and reaches the context as a Resize; the
	device, the sample count and every resource but the swap chain buffers and the scene target live on.
*/

#include "d3d12_Buffers.h"
#include "d3d12_Copies.h"
#include "d3d12_Draw.h"
#include "d3d12_Programs.h"
#include "d3d12_Textures.h"
#include "d3d12_Timers.h"

class idD3D12Context final : public idD3D12Submitter {
public:
	// Builds into a local object and returns it only complete: a failed Init leaves nothing behind.
	[[nodiscard]] static std::expected<std::unique_ptr<idD3D12Context>, idStr>	Create( const renderBackendInit_t & init );
									~idD3D12Context();	// WaitIdle (skipped when lost), CollectAll, the last messages
									idD3D12Context( const idD3D12Context & ) = delete;
	idD3D12Context &				operator=( const idD3D12Context & ) = delete;

	ID3D12GraphicsCommandList1 &	List() override;	// a new list gets the heap, the root signature and an empty bound state
	uint64							SubmitChunk() override;	// flushes the barriers; closes the chunks of the rings
	void							WaitFor( uint64 fence ) override;
	void							WaitIdle() override;
	uint64							PendingFence() const override { return queue.LastSignaled() + 1; }
	// The list that commands recorded now land in, also before it is opened.
	uint64							ListSerial() const override { return commands.IsOpen() ? commands.Serial() : commands.Serial() + 1; }
	uint64							WaitedFence() const override { return queue.Waited(); }

	void							BeginFrame( int width, int height );	// follows a window resized on its own, starts the frame timer
	void							EndFrame();								// ends the pass, resolves the timers, submits; no wait
	[[nodiscard]] presentResult_t	Present( presentMode_t mode );			// blit, submit, present, wait, retire, latency
	void							ReadOutputPixels( int x, int y, int width, int height, byte * rgb );	// R8: after EndFrame
	void							Resize( int width, int height );		// between frames; 0 x 0 (minimized) is ignored
	void							SetSrgb( bool enable );					// SRGB_OUTPUT: the scene's sRGB view for draws and clears
	void							SetGamma( const d3d12GammaRamp_t & ramp );	// the present blit's LUT; captures stay before it
	void							ShowImage( textureHandle_t texture );	// r_d3d12ShowImage into the scene, before EndFrame
	// glCopyTexImage2D of a scene box, in a frame; the next draw opens the pass again.
	void							CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height );
	void							CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height );
	d3d12TextureCheckResult_t		CheckTextures();						// d3d12_checkImages: between frames, waits
	void							PrintInfo() const;
	void							PushGroup( const char * name );			// r_pix 2, in a frame
	void							PopGroup();
	// The frame before the last Present.
	[[nodiscard]] const d3d12FrameStats_t &	LastFrameStats() const { return lastFrame; }

	idD3D12Library					library;
	idD3D12Device					device;
	idD3D12LiveObjectReport			liveObjects;
	idD3D12Queue					queue;
	idD3D12Graveyard				graveyard;
	idD3D12CommandStream			commands;
	idD3D12DebugGroups				groups;
	idD3D12GpuTimers				timers;
	idD3D12Barriers					barriers;
	idD3D12DescriptorPool			srvPool;
	idD3D12DescriptorPool			rtvPool;
	idD3D12DescriptorPool			dsvPool;
	idD3D12DescriptorRing			descriptors;
	idD3D12SamplerHeap				samplers;
	idD3D12UploadRing				constants;
	idD3D12UploadRing				staging;
	idD3D12Uploader					uploader;
	idD3D12BufferStore				buffers;
	idD3D12TextureStore				textures;
	idD3D12ShaderCompiler			compiler;
	idD3D12ShaderLibrary			shaders;
	idD3D12RootSignature			rootSignature;
	idD3D12PipelineFactory			pipelines;
	idD3D12SceneTarget				scene;
	std::optional<idD3D12SwapChain>	swapChain;
	idD3D12InternalPasses			passes;
	idD3D12Readback					readback;
	idD3D12ScreenCopies				copies;
	idD3D12DrawRecorder				recorder;

private:
									idD3D12Context( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice );
	[[nodiscard]] std::expected<void, idStr>	CreateOutput( HWND window, int width, int height, UINT samples );
	void							CreateDefaults();	// the fallback textures, the internal passes and the default buffers
	void							Retire( uint64 waitedFence );	// ring chunks and graves up to an explicitly waited fence
	// The scene, resolved, into the back buffer through the present blit; leaves it in RENDER_TARGET.
	void							BlitToBackBuffer( idD3D12SwapChain::backBuffer_t & back );
	void							EndFrameStats();	// at Present: the counters of the frame into lastFrame

	d3d12FrameStats_t				frame;
	d3d12FrameStats_t				lastFrame;
};
