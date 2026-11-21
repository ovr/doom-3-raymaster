// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	idD3D12Context: the composition root of the Direct3D 12 backend. Only the facade
	(idRenderBackendD3D12) knows it. It holds every subsystem by value; members are built in the
	order below and destroyed in the reverse order after WaitIdle, so a member may keep references
	to the members declared before it. The context lives on the heap and never moves.

	idRenderBackendD3D12                     the facade; renderBackend points at it
	 └── std::unique_ptr<idD3D12Context>      Init .. Shutdown; implements idD3D12Submitter
	      ├── idD3D12Library                  d3d12.dll, dxgi.dll from System32; dxcompiler.dll, dxil.dll from the exe's folder
	      ├── idD3D12Device                   IDXGIFactory6, IDXGIAdapter1, ID3D12Device2, info queues, features
	      ├── idD3D12LiveObjectReport         the teardown messages, Debug: what is alive once the members below are gone
	      ├── idD3D12Queue                    ID3D12CommandQueue, ID3D12Fence; Waited() = the last explicit wait
	      ├── idD3D12Graveyard                GPU objects that die after the fence of their last list
	      ├── idD3D12CommandStream            ID3D12GraphicsCommandList1 + allocators reused by fence
	      ├── idD3D12DebugGroups              r_pix 2: the group stack, ended at a submit and begun again in the next list
	      ├── idD3D12GpuTimers                r_pix 1 and the frame's GPU time: a TIMESTAMP query heap resolved in EndFrame
	      ├── idD3D12Barriers                 transitions batched until the next draw, clear, copy or submit
	      ├── idD3D12DescriptorPool x 3       SRV, RTV, DSV: the permanent views; the SRVs also in the one
	      │                                   shader-visible heap, which draws index (bindless.hlsli)
	      ├── idD3D12SamplerHeap              the shader-visible sampler heap, a slot per sampler, kept
	      ├── idD3D12UploadRing               "constants": 16 MB UPLOAD for the root CBVs, reused by fence
	      ├── idD3D12UploadRing               "staging": 64 MB UPLOAD, the copy sources of buffers and textures
	      ├── idD3D12Uploader                 copies through "staging" into the open list, a submit every 16 MB
	      ├── idD3D12BufferStore              bufferHandle_t → FRAME where r_d3d12FrameBuffers says, STATIC in DEFAULT + a CPU mirror
	      ├── idD3D12TextureStore             textureHandle_t → a committed texture + SRV, the black fallbacks
	      ├── idD3D12HlslCompiler             DXC over base/shaders/*.hlsl, the DXIL cache in shaders/cache
	      ├── idD3D12ShaderLibrary            shaderHandle_t, programHandle_t → DXIL stages, their PSOs
	      ├── idD3D12RootSignature            the 5 parameters of every PSO
	      ├── idD3D12ComputeRootSignature     b0 and b1 of the compute passes
	      ├── idD3D12PipelineJournal          every key drawn with, by program name: renderprogs/dxbc/pipelines.journal
	      ├── optional<idD3D12PipelineLibrary>  ID3D12PipelineLibrary1 over renderprogs/dxbc/pipelines.bin
	      ├── idD3D12PipelineFactory          normalized pipelineKey_t → a PSO kept with its program; the depth bias slots
	      ├── idD3D12SceneTarget              colour + depth in GL row order, the MSAA resolve texture
	      ├── optional<idD3D12SwapChain>      IDXGISwapChain3, 3 back buffers + RTVs, latency object
	      ├── idD3D12InternalPasses           the present blit with the gamma LUT, the masked clear, the depth copy, r_d3d12ShowImage
	      ├── idD3D12Readback                 copies into a READBACK buffer for ReadOutputPixels
	      ├── idD3D12ScreenCopies             CopyOutputToTexture, CopyDepthToTexture: scene boxes into textures
	      ├── idD3D12DrawRecorder             clears and draws into the open pass: Prepare, Reserve, Record
	      ├── idD3D12ResourcePool             the render graph's transient textures and buffers, kept 30 frames unused
	      ├── idD3D12RenderGraph              the post-process passes: culling, transient resources, barriers
	      └── idD3D12HdrPipeline              R13: the HDR scene's resolve on the graph, the combine LUT, the tonemap

	Sync points: every wait is explicit, and everything that frees or reuses memory reads
	idD3D12Queue::Waited(), never the fence, so two runs record the same commands.

	  point                          waits for                     why
	  Present                        the fence of the frame        the vertex cache remaps the set the frame read (R3, R8)
	  ReadOutputPixels               a signal after the copy       the CPU reads the readback buffer
	  a full ring                    the fence of its oldest chunk  that room is taken again
	  Resize, Shutdown               a signal after the last Present  ResizeBuffers and releasing a back buffer need an idle queue

	vid_restart only changes the window (R_SetNewMode( false )) and reaches the context as a Resize; the
	device, the sample count and every resource but the swap chain buffers and the scene target live on.

	PSOs: a draw's new key compiles its PSO in the frame and goes into the journal. The journal is written
	after each level load and at Shutdown, the library at Shutdown; the next run's CreateProgram makes the
	journal's PSOs of the program (from the library when it has them), so its frames compile none.
*/

#include "d3d12_Buffers.h"
#include "d3d12_Copies.h"
#include "d3d12_Draw.h"
#include "d3d12_Hdr.h"
#include "d3d12_Programs.h"
#include "d3d12_RenderGraph.h"
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
	// The library's CreateProgram, then the PSOs of the program's journal keys.
	[[nodiscard]] programHandle_t	CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * name, programInfo_t & info );
	void							EndLevelLoad();							// prints what the load's PSOs cost, writes the journal
	void							ShowImage( textureHandle_t texture );	// r_d3d12ShowImage into the scene, before EndFrame
	// glCopyTexImage2D of a scene box, in a frame; the next draw opens the pass again.
	void							CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height );
	void							CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height );
	// R13, in a frame: draws, clears and colour copies go to the HDR scene until its resolve or its end.
	void							BeginHdrScene( const postProcessSettings_t & settings );
	void							ResolveHdrScene( const postProcessSettings_t & settings, const renderRect_t & viewport, const hdrExposureStep_t & step );
	void							EndHdrScene();
	// The gamma composite of the view inside viewport: to the scene's sRGB encoding or back.
	void							SetHdrSceneGamma( bool gamma, const renderRect_t & viewport );
	[[nodiscard]] std::optional<hdrExposure_t>	LastHdrExposure() const;
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
	idD3D12SamplerHeap				samplers;
	idD3D12UploadRing				constants;
	idD3D12UploadRing				staging;
	idD3D12Uploader					uploader;
	idD3D12BufferStore				buffers;
	idD3D12TextureStore				textures;
	idD3D12HlslCompiler				hlslCompiler;
	idD3D12ShaderLibrary			shaders;
	idD3D12RootSignature			rootSignature;
	idD3D12ComputeRootSignature		computeRootSignature;
	idD3D12PipelineJournal			journal;
	std::optional<idD3D12PipelineLibrary>	pipelineLibrary;
	idD3D12PipelineFactory			pipelines;
	idD3D12SceneTarget				scene;
	std::optional<idD3D12SwapChain>	swapChain;
	idD3D12InternalPasses			passes;
	idD3D12Readback					readback;
	idD3D12ScreenCopies				copies;
	idD3D12DrawRecorder				recorder;
	idD3D12ResourcePool				resourcePool;
	idD3D12RenderGraph				graph;
	idD3D12HdrPipeline				hdr;

private:
									idD3D12Context( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice );
	[[nodiscard]] std::expected<void, idStr>	CreateOutput( HWND window, int width, int height, UINT samples );
	void							CreateDefaults();	// the fallback textures, the internal passes and the default buffers
	void							Retire( uint64 waitedFence );	// ring chunks and graves up to an explicitly waited fence
	// The scene, resolved, into the back buffer through the present blit; leaves it in RENDER_TARGET.
	void							BlitToBackBuffer( idD3D12SwapChain::backBuffer_t & back );
	void							EndFrameStats();	// at Present: the counters of the frame into lastFrame
	void							SavePipelines( bool withLibrary );	// the journal when it changed; the library too

	d3d12FrameStats_t				frame;
	d3d12FrameStats_t				lastFrame;
	bool							hdrScene = false;	// between BeginHdrScene and its resolve or end
	float							preExposure = 1.0f;
};
