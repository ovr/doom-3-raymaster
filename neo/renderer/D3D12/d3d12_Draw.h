// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include <functional>

#include "d3d12_Buffers.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_Textures.h"

/*
	A draw in three steps, so nothing submits once recording has begun:

	  Prepare   the normalized key and its PSO, the heap indices of the slots' views and samplers (the b3
	            root constants): no ring room, no command
	  Reserve   the parms in "constants"; a full ring submits, waits for its oldest chunk and tries again in
	            the new list
	  Record    the barriers of what the draw reads and the pass, then the PSO, the dynamic state, the root
	            parameters and DrawIndexedInstanced, each sent only when it differs from d3d12BoundState_t

	A list is submitted before its 65th draw of a frame, then before every r_d3d12SubmitDraws more, so the
	GPU starts on a frame while the CPU still records it. That submit comes before Prepare: no draw is cut.
*/

struct d3d12DrawStats_t {
	int								draws = 0;
	int								skipped = 0;		// program NONE or dead, a dead buffer, a lost device, no scissor left
	int								psoBinds = 0;
	int								resourceBinds = 0;	// b3 root constants set
	int								unboundSlots = 0;	// draws with a sampled slot without a texture of its kind (R11)
	int								incompleteBinds = 0;	// draws sampling a copy GL sees as incomplete, black (T6)
	int								chunkSubmits = 0;	// lists submitted because they had their draws
	int								reserveWaits = 0;	// full rings: Reserve waited for an older list
	uint64							parmBytes = 0;		// taken from "constants"
};

// What one frame cost, Present to Present: r_d3d12Stats sums these.
struct d3d12FrameStats_t {
	d3d12DrawStats_t				draw;
	int								psoCreated = 0;
	int								nearestBiases = 0;	// draws with the nearest depth bias of a full table
	int								submits = 0;		// ExecuteCommandLists
	int								waits = 0;			// explicit fence waits
	int								barriers = 0;		// transitions that reached a list
	uint64							uploadBytes = 0;	// copied through the staging ring
	uint64							inPlaceBytes = 0;	// written into FRAME buffers the GPU reads in place
	int								samplersMade = 0;	// sampler heap slots written
	int								debugGroups = 0;	// pushed
	int								timestamps = 0;		// r_pix slots written
	uint64							gpuMicroseconds = 0;	// the frame timer: BeginFrame to EndFrame on the GPU
	int								colorCopies = 0;	// CopyOutputToTexture
	int								depthCopies = 0;	// CopyDepthToTexture
	int								copySurfaces = 0;	// copy targets created for a new size
};

// What the open list has bound. Empty means unknown: a new list and every internal pass clear it.
struct d3d12BoundState_t {
	bool							passOpen = false;	// scene RTV + DSV set, colour in RENDER_TARGET, depth in DEPTH_WRITE
	std::optional<pipelineKey_t>	key;
	std::optional<primitive_t>		topology;
	std::optional<std::array<int, 4>>	viewport;		// x, y, width, height
	std::optional<std::array<int, 4>>	scissor;		// left, top, right, bottom inside the scene
	std::optional<uint8>			stencilRef;
	std::optional<std::pair<float, float>>	depthBounds;	// (0, 1) is the test off
	std::optional<std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2>>	parms;	// b0, b1
	programHandle_t					parmsProgram = programHandle_t::NONE;
	std::optional<D3D12_GPU_VIRTUAL_ADDRESS>	joints;		// b2: a joint buffer + jointOffset, or the zero block
	std::optional<d3d12DrawResources_t>	resources;	// b3
	std::optional<d3d12TraceResources_t>	trace;	// b4
	std::optional<std::pair<bufferHandle_t, uint8>>	vertexBuffer;	// and its layout, which gives the stride
	std::optional<bufferHandle_t>	indexBuffer;
	bool							defaultAttribs = false;	// slot 1
};

// The latch and the calls of the contract → commands into the open pass.
class idD3D12DrawRecorder {
public:
									idD3D12DrawRecorder( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12Barriers & barriers,
										idD3D12SceneTarget & scene, idD3D12InternalPasses & passes, idD3D12PipelineFactory & pipelines,
										idD3D12ShaderLibrary & shaders, idD3D12BufferStore & buffers, idD3D12TextureStore & textures,
										idD3D12UploadRing & constants, idD3D12SamplerHeap & samplers );
	// As glClear: inside the latched scissor, through the latched write masks; a partial colour mask is a pass.
	void							Clear( const renderLatch_t & latch, const clearDesc_t & clear );
	// key: idRenderBackend::PipelineKey; the target and the depth bias slot are set here.
	void							Draw( const renderLatch_t & latch, const pipelineKey_t & key, const drawCall_t & dc );
	void							SkippedDraw() { stats.skipped++; }	// R12: program NONE
	// GL_FRAMEBUFFER_SRGB: draws and clears write through the scene's sRGB view, which blends in linear space and
	// encodes. Copies, the readback and the blit move the stored bytes as they are. Ends the pass.
	void							SetSrgb( bool enable );
	[[nodiscard]] bool				Srgb() const { return srgb; }
	// R13: draws and clears go to the HDR scene, which holds radiance times preExposure. Ends the pass.
	void							SetHdr( bool enable, float preExposure );
	[[nodiscard]] bool				Hdr() const { return hdr; }
	// The gamma composite: the HDR scene holds its sRGB encoding, colours are not decoded. Ends the pass.
	void							SetHdrGamma( bool enable );
	[[nodiscard]] bool				HdrGamma() const { return hdr && hdrGamma; }
	// R14, in the linear phase: draws go to the G-buffer's targets with the scene's depth. Ends the pass.
	void							SetGBuffer( bool enable );
	[[nodiscard]] bool				GBuffer() const { return gbuffer; }
	// R16, in the linear phase: draws go to the HDR scene and the light target. Ends the pass.
	void							SetLights( bool enable );
	[[nodiscard]] bool				Lights() const { return lights; }
	// What b4 holds for a draw of a program that traces, asked per such draw: it queues the transitions of what the
	// rays read in the open list. Without a source, or while it returns nothing valid, such a draw traces nothing.
	using traceSource_t = std::function<d3d12TraceResources_t()>;
	void							SetTraceSource( traceSource_t source ) { traceSource = std::move( source ); }
	void							EndPass() { bound.passOpen = false; }	// R10: before a copy, a readback, the blit and in EndFrame
	void							Invalidate() { bound = {}; }			// after an internal pass
	void							NewList() { bound = {}; drawsInList = 0; }	// the submitter opened a list
	void							BeginFrame() { firstChunk = true; }
	void							ForgetBuffer( bufferHandle_t buffer );	// before it dies: a new one may get its handle
	void							ForgetProgram( programHandle_t program );	// before it dies: a new one may get its dense index
	[[nodiscard]] d3d12DrawStats_t	TakeStats();	// the frame's, and zero again

private:
	struct prepared_t {
		pipelineKey_t				key = {};
		ID3D12PipelineState *		pso = nullptr;
		d3d12DrawResources_t		resources;
		std::array<textureHandle_t, MAX_TEXTURE_SLOTS>	textures = {};	// the sampled ones; NONE reads a fallback
		uint32						vertexParmBytes = 0;
		uint32						fragmentParmBytes = 0;
		bool						defaultAttribs = false;
	};
	struct reserved_t {
		bool						parms = false;		// new parms: the spans of the stages that have any
		std::optional<d3d12UploadSpan_t>	vertexParms;
		std::optional<d3d12UploadSpan_t>	fragmentParms;
	};
	struct used_t {								// what the draw reads, its transitions queued
		const d3d12Buffer_t *		vertex = nullptr;
		const d3d12Buffer_t *		index = nullptr;
		std::optional<D3D12_GPU_VIRTUAL_ADDRESS>	joints;		// b2 of a program with usesJoints
		std::optional<d3d12TraceResources_t>	trace;		// b4 of a program that traces
	};

	[[nodiscard]] std::optional<prepared_t>	Prepare( const renderLatch_t & latch, const pipelineKey_t & key, const drawCall_t & dc,
										d3d12Program_t & program );
	[[nodiscard]] std::optional<reserved_t>	TryReserve( const prepared_t & prepared, const renderLatch_t & latch, const drawCall_t & dc );
	[[nodiscard]] reserved_t		Reserve( const prepared_t & prepared, const renderLatch_t & latch, const drawCall_t & dc );
	void							Record( ID3D12GraphicsCommandList4 & list, const prepared_t & prepared, const reserved_t & reserved,
										const renderLatch_t & latch, const drawCall_t & dc, const d3d12Program_t & program );
	void							BeginPass( ID3D12GraphicsCommandList4 & list );	// R10: the first draw or clear after EndPass
	// The latched scissor cut to the scene: left, top, right, bottom.
	[[nodiscard]] std::array<int, 4>	SceneScissor( const renderLatch_t & latch ) const;
	void							BindDynamicState( ID3D12GraphicsCommandList4 & list, const renderLatch_t & latch, primitive_t topology );
	void							BindParms( ID3D12GraphicsCommandList4 & list, const reserved_t & reserved, const renderLatch_t & latch,
										const drawCall_t & dc, const d3d12Program_t & program );
	// Queues the transitions, which Record flushes before it binds anything: the layer takes the state of a root
	// CBV's buffer when SetGraphicsRootConstantBufferView is recorded, so a barrier after it mismatches (#527).
	[[nodiscard]] used_t			Use( const prepared_t & prepared, const drawCall_t & dc, const d3d12Program_t & program );
	void							BindJoints( ID3D12GraphicsCommandList4 & list, std::optional<D3D12_GPU_VIRTUAL_ADDRESS> joints );
	void							BindResources( ID3D12GraphicsCommandList4 & list, const prepared_t & prepared, const d3d12Program_t & program );
	void							BindTrace( ID3D12GraphicsCommandList4 & list, const std::optional<d3d12TraceResources_t> & trace );
	void							BindGeometry( ID3D12GraphicsCommandList4 & list, const prepared_t & prepared, const drawCall_t & dc, const used_t & used );
	[[nodiscard]] int				ChunkLimit() const;	// the draws the open list takes before it is submitted

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12SceneTarget &			scene;
	idD3D12InternalPasses &			passes;
	idD3D12PipelineFactory &		pipelines;
	idD3D12ShaderLibrary &			shaders;
	idD3D12BufferStore &			buffers;
	idD3D12TextureStore &			textures;
	idD3D12UploadRing &				constants;
	idD3D12SamplerHeap &			samplers;
	traceSource_t					traceSource;
	d3d12BoundState_t				bound;
	d3d12DrawStats_t				stats;
	int								drawsInList = 0;
	bool							firstChunk = true;	// the frame has not submitted a full list yet
	bool							srgb = false;
	bool							hdr = false;
	bool							hdrGamma = false;
	bool							gbuffer = false;
	bool							lights = false;
	float							preExposure = 1.0f;
};
