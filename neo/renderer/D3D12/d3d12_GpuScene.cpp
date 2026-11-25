// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_GpuScene.h"

idD3D12GpuScene::idD3D12GpuScene( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12Graveyard & graveyard,
		idD3D12DescriptorPool & srvPool, idD3D12Barriers & barriers, idD3D12Uploader & uploader, idD3D12TextureStore & textures ) :
	device( device ),
	submitter( submitter ),
	graveyard( graveyard ),
	srvPool( srvPool ),
	barriers( barriers ),
	uploader( uploader ),
	textures( textures ) {
	state.decays = true;
	lightState.decays = true;
}

void idD3D12GpuScene::MakeTexturesReadable() {
	for ( const auto & [handle, uses] : textureUses ) {
		textures.MakeReadable( handle );
	}
	for ( const textureHandle_t handle : lightTextures ) {
		textures.MakeReadable( handle );
	}
}

bool idD3D12GpuScene::WriteLights( std::span<const gpuSceneLightWrite_t> lights ) {
	numLights = 0;
	lightTextures.clear();
	if ( lights.empty() ) {
		return true;
	}
	const int count = static_cast<int>( lights.size() );
	if ( count > lightCapacity ) {
		const int grownCapacity = Max( count, Max( lightCapacity * 2, 64 ) );
		const uint64 bytes = static_cast<uint64>( grownCapacity ) * sizeof( gpuSceneLight_t );
		idComPtr<ID3D12Resource> grown = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON, L"gpu scene lights" );
		if ( !grown ) {
			return false;
		}
		if ( lightBuffer ) {
			graveyard.Bury( std::move( lightBuffer ) );
			graveyard.Bury( srvPool, lightSrv );
		}
		lightBuffer = std::move( grown );
		lightState = d3d12ResourceState_t();
		lightState.decays = true;
		lightCapacity = grownCapacity;
		D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
		view.Format = DXGI_FORMAT_R32_TYPELESS;
		view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Buffer.NumElements = static_cast<UINT>( bytes / 4 );
		view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		lightSrv = srvPool.Allocate();
		srvPool.CreateSrv( lightSrv, *lightBuffer.Get(), view );
	}
	lightScratch.resize( lights.size() );
	for ( size_t i = 0; i < lights.size(); i++ ) {
		gpuSceneLight_t & light = lightScratch[i];
		light = lights[i].light;
		textureHandle_t sampled;
		light.projection = View( lights[i].textures[0], sampled );
		if ( sampled != textureHandle_t::NONE ) {
			lightTextures.push_back( sampled );
		}
		light.falloff = View( lights[i].textures[1], sampled );
		if ( sampled != textureHandle_t::NONE ) {
			lightTextures.push_back( sampled );
		}
	}
	uploader.BufferRange( *lightBuffer.Get(), lightState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0,
		std::span( reinterpret_cast<const byte *>( lightScratch.data() ), lightScratch.size() * sizeof( gpuSceneLight_t ) ) );
	numLights = count;
	return true;
}

uint32 idD3D12GpuScene::View( textureHandle_t handle, textureHandle_t & sampled ) {
	const d3d12Texture_t * texture = textures.Find( handle );
	if ( texture == nullptr || texture->role != textureRole_t::SAMPLED ) {
		sampled = textureHandle_t::NONE;
		return textures.Fallback( textureKind_t::TEX_2D ).index;
	}
	sampled = handle;
	return textures.SampledView( handle, textureKind_t::TEX_2D ).index;
}

void idD3D12GpuScene::SetRecordTexture( size_t record, int slot, textureHandle_t sampled ) {
	textureHandle_t & held = recordTextures[record][slot];
	if ( held == sampled ) {
		return;
	}
	if ( held != textureHandle_t::NONE ) {
		const auto found = textureUses.find( held );
		if ( --found->second == 0 ) {
			textureUses.erase( found );
		}
	}
	if ( sampled != textureHandle_t::NONE ) {
		textureUses[sampled]++;
	}
	held = sampled;
}

bool idD3D12GpuScene::Reserve( int records ) {
	if ( records <= capacity ) {
		return true;
	}
	const int grownCapacity = Max( records, capacity * 2 );
	const uint64 bytes = static_cast<uint64>( grownCapacity ) * sizeof( gpuSceneGeometry_t );
	idComPtr<ID3D12Resource> grown = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON, L"gpu scene geometries" );
	if ( !grown ) {
		return false;
	}
	d3d12ResourceState_t grownState;
	grownState.decays = true;
	if ( buffer ) {
		barriers.Transition( *buffer.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE );
		barriers.Transition( *grown.Get(), grownState, D3D12_RESOURCE_STATE_COPY_DEST );
		barriers.Flush();
		submitter.List().CopyBufferRegion( grown.Get(), 0, buffer.Get(), 0, static_cast<uint64>( capacity ) * sizeof( gpuSceneGeometry_t ) );
		graveyard.Bury( std::move( buffer ) );
		graveyard.Bury( srvPool, srv );
	}
	buffer = std::move( grown );
	state = grownState;
	capacity = grownCapacity;
	mirror.resize( static_cast<size_t>( capacity ) );
	recordTextures.resize( static_cast<size_t>( capacity ) );

	D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = DXGI_FORMAT_R32_TYPELESS;
	view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view.Buffer.NumElements = static_cast<UINT>( bytes / 4 );
	view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	srv = srvPool.Allocate();
	srvPool.CreateSrv( srv, *buffer.Get(), view );
	return true;
}

bool idD3D12GpuScene::Update( const gpuSceneUpdate_t & update ) {
	const gpuSceneFrame_t & frame = *update.frame;
	if ( device.IsLost() ) {
		valid = false;
		return false;
	}
	if ( valid && frame.sceneId == sceneId && frame.sequence == sequence ) {
		return true;
	}
	const bool follows = valid && frame.sceneId == sceneId && frame.sequence == sequence + 1
		&& frame.staticGeneration == staticGeneration && textures.SampledDestroyed() == sampledDestroyed;
	if ( ( !frame.full && !follows ) || !Reserve( frame.geometryCapacity ) || !WriteLights( std::span( frame.lights, static_cast<size_t>( frame.numLights ) ) ) ) {
		valid = false;
		instances.clear();
		numLights = 0;
		return false;
	}
	if ( frame.full ) {
		staticGeneration = frame.staticGeneration;
		sampledDestroyed = textures.SampledDestroyed();
	}

	// one copy per run of consecutive records
	stats = {};
	stats.full = frame.full;
	stats.writes = frame.numWrites;
	for ( int first = 0; first < frame.numWrites; ) {
		int end = first + 1;
		while ( end < frame.numWrites && frame.writes[end].index == frame.writes[end - 1].index + 1 ) {
			end++;
		}
		scratch.resize( static_cast<size_t>( end - first ) * sizeof( gpuSceneGeometry_t ) );
		for ( int i = first; i < end; i++ ) {
			const gpuSceneWrite_t & write = frame.writes[i];
			assert( write.index >= 0 && write.index < capacity );
			gpuSceneGeometry_t & record = mirror[static_cast<size_t>( write.index )];
			record = write.record;
			for ( int t = 0; t < GPU_SCENE_TEXTURES; t++ ) {
				textureHandle_t sampled;
				record.textures[t] = View( write.textures[t], sampled );
				SetRecordTexture( static_cast<size_t>( write.index ), t, sampled );
			}
			memcpy( scratch.data() + static_cast<size_t>( i - first ) * sizeof( gpuSceneGeometry_t ), &record, sizeof( record ) );
		}
		uploader.BufferRange( *buffer.Get(), state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			static_cast<uint64>( frame.writes[first].index ) * sizeof( gpuSceneGeometry_t ), scratch );
		stats.copies++;
		stats.bytes += scratch.size();
		first = end;
	}

	instances.assign( frame.instances, frame.instances + frame.numInstances );
	staticVertexBuffer = update.staticVertexBuffer;
	staticIndexBuffer = update.staticIndexBuffer;
	sceneId = frame.sceneId;
	sequence = frame.sequence;
	valid = true;
	return true;
}
