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

#include "d3d12_Buffers.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_Textures.h"

/*
	A draw in three steps, so nothing submits once recording has begun:

	  Prepare   the normalized key and its PSO, the views and samplers of the slots: no ring room, no command
	  Reserve   the room the open list lacks: the parms in "constants", an SRV table, a sampler table; a full
	            ring submits, waits for its oldest chunk and tries again in the new list
	  Record    the pass, the PSO, the dynamic state, the root parameters, the barriers, DrawIndexedInstanced,
	            each sent only when it differs from d3d12BoundState_t

	Joints are the zero block and depth bounds stay (0, 1) until the stages that bring them.
*/

struct d3d12DrawStats_t {
	int								draws = 0;
	int								skipped = 0;		// program NONE or dead, a dead buffer, a lost device, no scissor left
	int								psoBinds = 0;
	int								tableCopies = 0;	// SRV tables copied into the ring
	int								incompleteBinds = 0;	// draws with a sampled slot that reads black (R11)
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
	int								samplerTables = 0;	// sampler tables built
	int								debugGroups = 0;	// pushed
};

// The SRV pool slots and the packed samplers of the 8 slots of a draw.
struct d3d12TextureSignature_t {
	std::array<uint32, MAX_TEXTURE_SLOTS>	srvs = {};
	d3d12SamplerTableKey_t			samplers;
};

// What the open list has bound. Empty means unknown: a new list and every internal pass clear it.
struct d3d12BoundState_t {
	bool							passOpen = false;	// scene RTV + DSV set, colour in RENDER_TARGET, depth in DEPTH_WRITE
	std::optional<pipelineKey_t>	key;
	std::optional<primitive_t>		topology;
	std::optional<std::array<int, 4>>	viewport;		// x, y, width, height
	std::optional<std::array<int, 4>>	scissor;		// left, top, right, bottom inside the scene
	std::optional<uint8>			stencilRef;
	bool							depthBounds = false;	// (0, 1)
	std::optional<std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2>>	parms;	// b0, b1
	programHandle_t					parmsProgram = programHandle_t::NONE;
	bool							joints = false;		// the zero block
	std::optional<std::array<uint32, MAX_TEXTURE_SLOTS>>	srvs;
	std::optional<d3d12SamplerTableKey_t>	samplers;
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
										idD3D12DescriptorPool & srvPool, idD3D12UploadRing & constants, idD3D12DescriptorRing & descriptors,
										idD3D12SamplerHeap & samplers );
	// As glClear: inside the latched scissor, through the latched write masks; a partial colour mask is a pass.
	void							Clear( const renderLatch_t & latch, const clearDesc_t & clear );
	// key: idRenderBackend::PipelineKey; the target and the depth bias slot are added here.
	void							Draw( const renderLatch_t & latch, const pipelineKey_t & key, const drawCall_t & dc );
	void							SkippedDraw() { stats.skipped++; }	// R12: program NONE
	void							EndPass() { bound.passOpen = false; }	// R10: before a copy, a readback, the blit and in EndFrame
	void							Invalidate() { bound = {}; }			// a new list or an internal pass
	void							ForgetBuffer( bufferHandle_t buffer );	// before it dies: a new one may get its handle
	void							ForgetProgram( programHandle_t program );	// before it dies: a new one may get its dense index
	[[nodiscard]] d3d12DrawStats_t	TakeStats();	// the frame's, and zero again

private:
	struct prepared_t {
		pipelineKey_t				key = {};
		ID3D12PipelineState *		pso = nullptr;
		d3d12TextureSignature_t		signature;
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MAX_TEXTURE_SLOTS>	sources = {};
		std::array<textureHandle_t, MAX_TEXTURE_SLOTS>	textures = {};	// the sampled ones; NONE reads a fallback
		uint32						vertexParmBytes = 0;
		uint32						fragmentParmBytes = 0;
		bool						defaultAttribs = false;
	};
	struct reserved_t {
		bool						parms = false;		// new parms: the spans of the stages that have any
		std::optional<d3d12UploadSpan_t>	vertexParms;
		std::optional<d3d12UploadSpan_t>	fragmentParms;
		std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>	table;
		std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>	samplerTable;
	};

	[[nodiscard]] std::optional<prepared_t>	Prepare( const renderLatch_t & latch, const pipelineKey_t & key, const drawCall_t & dc,
										d3d12Program_t & program );
	[[nodiscard]] std::optional<reserved_t>	TryReserve( const prepared_t & prepared, const renderLatch_t & latch, const drawCall_t & dc );
	[[nodiscard]] reserved_t		Reserve( const prepared_t & prepared, const renderLatch_t & latch, const drawCall_t & dc );
	void							Record( ID3D12GraphicsCommandList1 & list, const prepared_t & prepared, const reserved_t & reserved,
										const renderLatch_t & latch, const drawCall_t & dc, const d3d12Program_t & program );
	void							BeginPass( ID3D12GraphicsCommandList1 & list );	// R10: the first draw or clear after EndPass
	// The latched scissor cut to the scene: left, top, right, bottom.
	[[nodiscard]] std::array<int, 4>	SceneScissor( const renderLatch_t & latch ) const;
	void							BindDynamicState( ID3D12GraphicsCommandList1 & list, const renderLatch_t & latch, primitive_t topology );
	void							BindParms( ID3D12GraphicsCommandList1 & list, const reserved_t & reserved, const renderLatch_t & latch,
										const drawCall_t & dc, const d3d12Program_t & program );
	void							BindTextures( ID3D12GraphicsCommandList1 & list, const prepared_t & prepared, const reserved_t & reserved );
	void							BindGeometry( ID3D12GraphicsCommandList1 & list, const prepared_t & prepared, const drawCall_t & dc );

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12SceneTarget &			scene;
	idD3D12InternalPasses &			passes;
	idD3D12PipelineFactory &		pipelines;
	idD3D12ShaderLibrary &			shaders;
	idD3D12BufferStore &			buffers;
	idD3D12TextureStore &			textures;
	idD3D12DescriptorPool &			srvPool;
	idD3D12UploadRing &				constants;
	idD3D12DescriptorRing &			descriptors;
	idD3D12SamplerHeap &			samplers;
	d3d12BoundState_t				bound;
	d3d12DrawStats_t				stats;
};
