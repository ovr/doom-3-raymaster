// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Draw.h"

idCVar r_d3d12SubmitDraws( "r_d3d12SubmitDraws", "256", CVAR_RENDERER | CVAR_INTEGER,
	"Direct3D 12 submits the open list after the first 64 draws of a frame, then after every N more; 0 = only at the end of a frame and on full rings",
	0, 65536 );

namespace {

// Small, so the GPU starts early in the frame; r_d3d12SubmitDraws takes over after it.
constexpr int FIRST_CHUNK_DRAWS = 64;

// A root CBV reads its cbuffer whole: the parms a stage packs, in 256-byte steps of the ring.
[[nodiscard]] constexpr uint32 ParmBytes( const shaderStageInfo_t & info ) {
	const uint32 bytes = static_cast<uint32>( info.numParms ) * static_cast<uint32>( sizeof( idVec4 ) );
	return ( bytes + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1 ) & ~( D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1 );
}

}	// namespace

idD3D12DrawRecorder::idD3D12DrawRecorder( const idD3D12Device & owner, idD3D12Submitter & submit, idD3D12Barriers & batch,
	idD3D12SceneTarget & target, idD3D12InternalPasses & internalPasses, idD3D12PipelineFactory & factory, idD3D12ShaderLibrary & library,
	idD3D12BufferStore & bufferStore, idD3D12TextureStore & textureStore, idD3D12UploadRing & constantRing, idD3D12SamplerHeap & samplerHeap ) :
	device( owner ),
	submitter( submit ),
	barriers( batch ),
	scene( target ),
	passes( internalPasses ),
	pipelines( factory ),
	shaders( library ),
	buffers( bufferStore ),
	textures( textureStore ),
	constants( constantRing ),
	samplers( samplerHeap ) {
}

void idD3D12DrawRecorder::BeginPass( ID3D12GraphicsCommandList1 & list ) {
	idD3D12SceneTarget::surface_t & color = scene.Color();
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	barriers.Transition( *depth.resource.Get(), depth.state, D3D12_RESOURCE_STATE_DEPTH_WRITE );
	barriers.Flush();
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.Rtv( srgb );
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = scene.Dsv();
	list.OMSetRenderTargets( 1, &rtv, FALSE, &dsv );
	bound.passOpen = true;
}

// The target id in the key changes with the view, so the next draw binds a PSO of the other format.
void idD3D12DrawRecorder::SetSrgb( bool enable ) {
	if ( srgb != enable ) {
		srgb = enable;
		EndPass();
	}
}

// The scene is in GL row order, so the rectangle of shared code is used as it is. glClear obeys
// glDepthMask; the stencil write mask is never changed from 0xFF.
void idD3D12DrawRecorder::Clear( const renderLatch_t & latch, const clearDesc_t & clear ) {
	const int x0 = Max( clear.rect.x, 0 );
	const int y0 = Max( clear.rect.y, 0 );
	const int x1 = Min( clear.rect.x + clear.rect.width, scene.Width() );
	const int y1 = Min( clear.rect.y + clear.rect.height, scene.Height() );
	if ( x1 <= x0 || y1 <= y0 ) {
		return;
	}
	const uint8 mask = clear.color ? static_cast<uint8>( clear.colorWriteMask & 0xF ) : 0;
	D3D12_CLEAR_FLAGS depthStencil = {};
	if ( clear.depth && ( latch.stateBits & GLS_DEPTHMASK ) == 0 ) {
		depthStencil |= D3D12_CLEAR_FLAG_DEPTH;
	}
	if ( clear.stencil ) {
		depthStencil |= D3D12_CLEAR_FLAG_STENCIL;
	}

	if ( mask == 0xF || depthStencil != 0 ) {
		ID3D12GraphicsCommandList1 & list = submitter.List();
		if ( !bound.passOpen ) {
			BeginPass( list );
		}
		const D3D12_RECT rect = { x0, y0, x1, y1 };
		if ( mask == 0xF ) {
			list.ClearRenderTargetView( scene.Rtv( srgb ), clear.rgba, 1, &rect );
		}
		if ( depthStencil != 0 ) {
			list.ClearDepthStencilView( scene.Dsv(), depthStencil, 1.0f, clear.stencilValue, 1, &rect );
		}
	}
	if ( mask != 0 && mask != 0xF ) {
		idD3D12SceneTarget::surface_t & color = scene.Color();
		barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		passes.MaskedClear( scene.Rtv( srgb ), scene.Target( srgb ), { x0, y0, x1 - x0, y1 - y0 }, clear.rgba, mask );
		Invalidate();
	}
}

// The list is opened first: what Reserve finds bound must be what the draw is recorded after.
// A scissor outside the scene (the first frame after a resize still has the old size) writes nothing
// on GL either; D3D12 warns about it (#695).
void idD3D12DrawRecorder::Draw( const renderLatch_t & latch, const pipelineKey_t & key, const drawCall_t & dc ) {
	d3d12Program_t * program = shaders.FindProgram( latch.program );
	const std::array<int, 4> scissor = SceneScissor( latch );
	if ( program == nullptr || device.IsLost() || scissor[0] == scissor[2] || scissor[1] == scissor[3] ) {
		stats.skipped++;
		return;
	}
	if ( drawsInList >= ChunkLimit() ) {
		submitter.SubmitChunk();
		firstChunk = false;
		stats.chunkSubmits++;
	}
	static_cast<void>( submitter.List() );
	const std::optional<prepared_t> prepared = Prepare( latch, key, dc, *program );
	if ( !prepared ) {
		stats.skipped++;
		return;
	}
	const reserved_t reserved = Reserve( *prepared, latch, dc );
	Record( submitter.List(), *prepared, reserved, latch, dc, *program );
	drawsInList++;
	stats.draws++;
}

int idD3D12DrawRecorder::ChunkLimit() const {
	const int submitDraws = r_d3d12SubmitDraws.GetInteger();
	if ( submitDraws <= 0 ) {
		return INT_MAX;
	}
	return firstChunk ? FIRST_CHUNK_DRAWS : submitDraws;
}

std::optional<idD3D12DrawRecorder::prepared_t> idD3D12DrawRecorder::Prepare( const renderLatch_t & latch, const pipelineKey_t & sharedKey,
	const drawCall_t & dc, d3d12Program_t & program ) {
	assert( dc.parms != nullptr );
	if ( dc.layout == LAYOUT_UNKNOWN || dc.layout >= NUM_VERTEX_LAYOUTS || buffers.Find( dc.vertexBuffer ) == nullptr
			|| buffers.Find( dc.indexBuffer ) == nullptr ) {
		return std::nullopt;
	}
	prepared_t prepared;
	pipelineKey_t key = sharedKey;
	key.program = program.dense;
	key.raster = static_cast<uint8>( key.raster | ( scene.Target( srgb ).value << D3D12_KEY_TARGET_SHIFT ) );
	if ( ( latch.stateBits & GLS_POLYGON_OFFSET ) != 0 && dc.topology == primitive_t::TRIANGLES ) {
		key.depthBias = pipelines.Bias().Slot( latch.polyOfsScale, latch.polyOfsBias );
	}
	prepared.key = D3D12_NormalizeKey( key );
	prepared.pso = pipelines.Get( program, prepared.key );
	if ( prepared.pso == nullptr ) {
		return std::nullopt;
	}
	prepared.defaultAttribs = ( program.info.attribMask & ~vertexLayouts[dc.layout].attribMask ) != 0;

	// R11: a sampled slot without a texture of its kind reads the black fallback, with sampler 0; so does an
	// incomplete copy (T6), with its own sampler
	bool unbound = false;
	bool incomplete = false;
	for ( int slot = 0; slot < MAX_TEXTURE_SLOTS; slot++ ) {
		const int bit = 1 << slot;
		if ( ( program.info.samplerMask & bit ) == 0 ) {
			continue;
		}
		const bool cube = ( program.info.cubeMask & bit ) != 0;
		const textureKind_t kind = cube ? textureKind_t::TEX_CUBE : textureKind_t::TEX_2D;
		textureHandle_t handle = latch.textures[slot][static_cast<size_t>( kind )];
		const d3d12Texture_t * texture = textures.Find( handle );
		uint32 sampler = 0;
		if ( texture != nullptr && texture->kind == kind ) {
			sampler = texture->samplerSlot;
			incomplete |= texture->IncompleteCopy();
		} else {
			handle = textureHandle_t::NONE;
			sampler = samplers.FallbackSlot();
			unbound = true;
		}
		prepared.textures[slot] = handle;
		prepared.resources.SetTexture( slot, cube, textures.SampledView( handle, kind ).index );
		prepared.resources.SetSampler( slot, sampler );
	}
	stats.unboundSlots += unbound ? 1 : 0;
	stats.incompleteBinds += incomplete ? 1 : 0;
	prepared.vertexParmBytes = ParmBytes( program.vertexInfo );
	prepared.fragmentParmBytes = ParmBytes( program.fragmentInfo );
	return prepared;
}

std::optional<idD3D12DrawRecorder::reserved_t> idD3D12DrawRecorder::TryReserve( const prepared_t & prepared, const renderLatch_t & latch,
	const drawCall_t & dc ) {
	reserved_t reserved;
	reserved.parms = !dc.reuseParms || !bound.parms || bound.parmsProgram != latch.program;
	if ( reserved.parms ) {
		if ( prepared.vertexParmBytes > 0 ) {
			reserved.vertexParms = constants.TryAllocate( prepared.vertexParmBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT );
			if ( !reserved.vertexParms ) {
				return std::nullopt;
			}
		}
		if ( prepared.fragmentParmBytes > 0 ) {
			reserved.fragmentParms = constants.TryAllocate( prepared.fragmentParmBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT );
			if ( !reserved.fragmentParms ) {
				return std::nullopt;	// what was taken stays unused in the chunk SubmitChunk closes
			}
		}
	}
	return reserved;
}

// A full ring: submit, wait for the oldest chunk, and take again in the new list, whose bound state is empty.
idD3D12DrawRecorder::reserved_t idD3D12DrawRecorder::Reserve( const prepared_t & prepared, const renderLatch_t & latch, const drawCall_t & dc ) {
	std::optional<reserved_t> reserved = TryReserve( prepared, latch, dc );
	while ( !reserved ) {
		submitter.SubmitChunk();
		const uint64 oldest = constants.OldestFence();
		if ( oldest == 0 ) {
			common->FatalError( "d3d12: a draw does not fit into an empty ring" );
		}
		submitter.WaitFor( oldest );
		stats.reserveWaits++;
		static_cast<void>( submitter.List() );
		reserved = TryReserve( prepared, latch, dc );
	}
	return *reserved;
}

// The draw's barriers go into the list with the pass's, in one batch, before anything is bound.
void idD3D12DrawRecorder::Record( ID3D12GraphicsCommandList1 & list, const prepared_t & prepared, const reserved_t & reserved,
	const renderLatch_t & latch, const drawCall_t & dc, const d3d12Program_t & program ) {
	const used_t used = Use( prepared, dc, program );
	if ( !bound.passOpen ) {
		BeginPass( list );
	}
	barriers.Flush();
	if ( bound.key != prepared.key ) {
		list.SetPipelineState( prepared.pso );
		bound.key = prepared.key;
		stats.psoBinds++;
	}
	BindDynamicState( list, latch, dc.topology );
	BindParms( list, reserved, latch, dc, program );
	BindJoints( list, used.joints );
	BindResources( list, prepared, program );
	BindGeometry( list, prepared, dc, used );
	list.DrawIndexedInstanced( static_cast<UINT>( dc.numIndexes ), 1, static_cast<UINT>( dc.indexOffset / static_cast<int>( sizeof( triIndex_t ) ) ),
		dc.baseVertex, 0 );
}

std::array<int, 4> idD3D12DrawRecorder::SceneScissor( const renderLatch_t & latch ) const {
	const renderRect_t & s = latch.scissor;
	const int left = idMath::ClampInt( 0, scene.Width(), s.x );
	const int top = idMath::ClampInt( 0, scene.Height(), s.y );
	return { left, top, idMath::ClampInt( left, scene.Width(), s.x + s.width ), idMath::ClampInt( top, scene.Height(), s.y + s.height ) };
}

// GL's rectangles in GL row order are the scene's; the scissor is cut to the scene, as D3D12 asks.
void idD3D12DrawRecorder::BindDynamicState( ID3D12GraphicsCommandList1 & list, const renderLatch_t & latch, primitive_t topology ) {
	if ( bound.topology != topology ) {
		list.IASetPrimitiveTopology( ( topology == primitive_t::LINES ) ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		bound.topology = topology;
	}
	const renderRect_t & v = latch.viewport;
	const std::array<int, 4> viewport = { v.x, v.y, v.width, v.height };
	if ( bound.viewport != viewport ) {
		const D3D12_VIEWPORT view = { static_cast<float>( v.x ), static_cast<float>( v.y ), static_cast<float>( v.width ), static_cast<float>( v.height ),
			0.0f, 1.0f };
		list.RSSetViewports( 1, &view );
		bound.viewport = viewport;
	}
	const std::array<int, 4> scissor = SceneScissor( latch );
	if ( bound.scissor != scissor ) {
		const D3D12_RECT rect = { scissor[0], scissor[1], scissor[2], scissor[3] };
		list.RSSetScissorRects( 1, &rect );
		bound.scissor = scissor;
	}
	const uint8 stencilRef = static_cast<uint8>( ( latch.stateBits & GLS_STENCIL_FUNC_REF_BITS ) >> GLS_STENCIL_FUNC_REF_SHIFT );
	if ( bound.stencilRef != stencilRef ) {
		list.OMSetStencilRef( stencilRef );
		bound.stencilRef = stencilRef;
	}
	if ( device.Features().depthBounds ) {
		// Every PSO has the test on, so GL's off, (0, 0), is (0, 1). Clamped as glDepthBoundsEXT does: D3D12 rejects bounds outside [0, 1].
		std::pair<float, float> depthBounds( 0.0f, 1.0f );
		if ( latch.depthBoundsMin != 0.0f || latch.depthBoundsMax != 0.0f ) {
			depthBounds = { idMath::ClampFloat( 0.0f, 1.0f, latch.depthBoundsMin ), idMath::ClampFloat( 0.0f, 1.0f, latch.depthBoundsMax ) };
		}
		if ( bound.depthBounds != depthBounds ) {
			list.OMSetDepthBounds( depthBounds.first, depthBounds.second );
			bound.depthBounds = depthBounds;
		}
	}
}

// R7: reuseParms keeps the spans of the draw before. A stage without parms reads the zero block.
void idD3D12DrawRecorder::BindParms( ID3D12GraphicsCommandList1 & list, const reserved_t & reserved, const renderLatch_t & latch,
	const drawCall_t & dc, const d3d12Program_t & program ) {
	if ( !reserved.parms ) {
		return;
	}
	std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2> parms = { buffers.ZeroJoints(), buffers.ZeroJoints() };
	if ( reserved.vertexParms ) {
		R_PackRenderParms( reinterpret_cast<idVec4 *>( reserved.vertexParms->cpu ), dc.parms, program.vertexInfo );
		parms[0] = reserved.vertexParms->gpu;
	}
	if ( reserved.fragmentParms ) {
		R_PackRenderParms( reinterpret_cast<idVec4 *>( reserved.fragmentParms->cpu ), dc.parms, program.fragmentInfo );
		parms[1] = reserved.fragmentParms->gpu;
	}
	stats.parmBytes += ( reserved.vertexParms ? ParmBytes( program.vertexInfo ) : 0 ) + ( reserved.fragmentParms ? ParmBytes( program.fragmentInfo ) : 0 );
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::VERTEX_PARMS ), parms[0] );
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::FRAGMENT_PARMS ), parms[1] );
	bound.parms = parms;
	bound.parmsProgram = latch.program;
}

// A STAGED buffer decays to COMMON with every list, so it transitions in each list it is drawn from, also when it
// stays bound, and an upload may have put a texture back into COPY_DEST. A skinned program drawn without joints
// (heatHaze with rpEnableSkinning 0) reads the zero block.
idD3D12DrawRecorder::used_t idD3D12DrawRecorder::Use( const prepared_t & prepared, const drawCall_t & dc, const d3d12Program_t & program ) {
	used_t used;
	used.vertex = buffers.Use( dc.vertexBuffer );
	used.index = buffers.Use( dc.indexBuffer );
	assert( used.vertex != nullptr && used.index != nullptr );
	if ( program.info.usesJoints ) {
		used.joints = buffers.ZeroJoints();
		if ( const d3d12Buffer_t * joints = buffers.Use( dc.jointBuffer ) ) {
			assert( dc.jointOffset >= 0 && dc.jointOffset < joints->size && ( dc.jointOffset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT ) == 0 );
			used.joints = joints->gpu + static_cast<uint64>( dc.jointOffset );
		}
	}
	for ( const textureHandle_t texture : prepared.textures ) {
		if ( texture != textureHandle_t::NONE ) {
			textures.MakeReadable( texture );
		}
	}
	return used;
}

// Root CBV b2, which the JOINT buffer's zero tail keeps readable for all 408 vectors; nothing for other programs.
void idD3D12DrawRecorder::BindJoints( ID3D12GraphicsCommandList1 & list, std::optional<D3D12_GPU_VIRTUAL_ADDRESS> joints ) {
	if ( joints && bound.joints != joints ) {
		list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::JOINTS ), *joints );
		bound.joints = joints;
	}
}

// A program that samples nothing does not read b3, so it keeps what the last one set.
void idD3D12DrawRecorder::BindResources( ID3D12GraphicsCommandList1 & list, const prepared_t & prepared, const d3d12Program_t & program ) {
	if ( program.info.samplerMask != 0 && bound.resources != prepared.resources ) {
		list.SetGraphicsRoot32BitConstants( D3D12_Root( d3d12RootSlot_t::DRAW_RESOURCES ), d3d12DrawResources_t::COUNT,
			prepared.resources.indices.data(), 0 );
		bound.resources = prepared.resources;
		stats.resourceBinds++;
	}
}

void idD3D12DrawRecorder::BindGeometry( ID3D12GraphicsCommandList1 & list, const prepared_t & prepared, const drawCall_t & dc, const used_t & used ) {
	const std::pair<bufferHandle_t, uint8> vertexBuffer( dc.vertexBuffer, static_cast<uint8>( dc.layout ) );
	if ( bound.vertexBuffer != vertexBuffer ) {
		const D3D12_VERTEX_BUFFER_VIEW view = { used.vertex->gpu, static_cast<UINT>( used.vertex->size ), vertexLayouts[dc.layout].stride };
		list.IASetVertexBuffers( 0, 1, &view );
		bound.vertexBuffer = vertexBuffer;
	}
	if ( prepared.defaultAttribs && !bound.defaultAttribs ) {
		const D3D12_VERTEX_BUFFER_VIEW defaults = buffers.DefaultAttribs();
		list.IASetVertexBuffers( 1, 1, &defaults );
		bound.defaultAttribs = true;
	}
	if ( bound.indexBuffer != dc.indexBuffer ) {
		const D3D12_INDEX_BUFFER_VIEW view = { used.index->gpu, static_cast<UINT>( used.index->size ), DXGI_FORMAT_R16_UINT };
		list.IASetIndexBuffer( &view );
		bound.indexBuffer = dc.indexBuffer;
	}
}

void idD3D12DrawRecorder::ForgetBuffer( bufferHandle_t buffer ) {
	if ( bound.vertexBuffer && bound.vertexBuffer->first == buffer ) {
		bound.vertexBuffer.reset();
	}
	if ( bound.indexBuffer == buffer ) {
		bound.indexBuffer.reset();
	}
}

void idD3D12DrawRecorder::ForgetProgram( programHandle_t program ) {
	if ( bound.parmsProgram == program ) {
		bound.parms.reset();
		bound.parmsProgram = programHandle_t::NONE;
	}
	bound.key.reset();
}

d3d12DrawStats_t idD3D12DrawRecorder::TakeStats() {
	const d3d12DrawStats_t taken = stats;
	stats = {};
	return taken;
}
