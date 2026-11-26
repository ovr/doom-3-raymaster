// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_RtHistory.h"

namespace {

constexpr std::array<const wchar_t *, static_cast<size_t>( rtHistorySlot_t::COUNT )> NAMES = {
	L"doom3 rt history guides", L"doom3 rt history shadow colour", L"doom3 rt history shadow aux",
	L"doom3 rt history indirect guides", L"doom3 rt history indirect amount", L"doom3 rt history indirect aux",
	L"doom3 rt history reflection colour", L"doom3 rt history reflection aux"
};

[[nodiscard]] uint32 BytesPerTexel( DXGI_FORMAT format ) {
	switch ( format ) {
		case DXGI_FORMAT_R32G32B32A32_FLOAT:	return 16;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:	return 8;
		default:								return 4;
	}
}

}	// namespace

idD3D12RtHistory::idD3D12RtHistory( const idD3D12Device & owner, idD3D12Graveyard & graves, idD3D12DescriptorPool & srvs ) :
	device( owner ),
	graveyard( graves ),
	srvPool( srvs ) {
}

idD3D12RtHistory::use_t idD3D12RtHistory::Use( rtHistorySlot_t slot, DXGI_FORMAT format, uint32 width, uint32 height, const rtView_t & view ) {
	pair_t & pair = eyes[view.eye & 1][static_cast<size_t>( slot )];
	if ( !pair.surfaces[0].resource || !pair.surfaces[1].resource || pair.format != format || pair.width != width || pair.height != height ) {
		Bury( pair );
		pair.format = format;
		pair.width = width;
		pair.height = height;
		if ( width == 0 || height == 0 || device.IsLost() || !Create( pair, slot ) ) {
			Bury( pair );
			return {};
		}
	}
	int current = -1;
	int previous = -1;
	for ( int i = 0; i < 2; i++ ) {
		if ( !pair.held[i] || pair.epoch[i] != view.epoch ) {
			continue;
		}
		if ( pair.sequence[i] == view.sequence ) {
			current = i;
		} else if ( view.sequence > 0 && pair.sequence[i] == view.sequence - 1 ) {
			previous = i;
		}
	}
	// the same sequence again writes the surface it wrote
	pair.next = ( current >= 0 ) ? current : ( ( previous >= 0 ) ? 1 - previous : 0 );
	use_t use;
	use.previous = ( previous >= 0 ) ? &pair.surfaces[previous] : nullptr;
	use.next = &pair.surfaces[pair.next];
	return use;
}

void idD3D12RtHistory::Written( rtHistorySlot_t slot, const rtView_t & view ) {
	pair_t & pair = eyes[view.eye & 1][static_cast<size_t>( slot )];
	if ( pair.surfaces[pair.next].resource ) {
		pair.held[pair.next] = true;
		pair.epoch[pair.next] = view.epoch;
		pair.sequence[pair.next] = view.sequence;
	}
}

void idD3D12RtHistory::Drop() {
	for ( auto & eye : eyes ) {
		for ( pair_t & pair : eye ) {
			Bury( pair );
		}
	}
}

// A submitted list may still read the views: they go through the graveyard with their resources.
void idD3D12RtHistory::Bury( pair_t & pair ) {
	for ( surface_t & surface : pair.surfaces ) {
		if ( surface.resource ) {
			bytes -= static_cast<uint64>( pair.width ) * pair.height * BytesPerTexel( pair.format );
			graveyard.Bury( std::move( surface.resource ) );
		}
		for ( d3d12Descriptor_t * view : { &surface.srv, &surface.uav } ) {
			if ( view->IsValid() ) {
				graveyard.Bury( srvPool, *view );
				*view = {};
			}
		}
		surface.state = {};
	}
	pair.held = {};
}

bool idD3D12RtHistory::Create( pair_t & pair, rtHistorySlot_t slot ) {
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = pair.width;
	desc.Height = pair.height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = pair.format;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = pair.format;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.Format = pair.format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	for ( surface_t & surface : pair.surfaces ) {
		surface.state.current = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		if ( !device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, surface.state.current, nullptr,
				IID_PPV_ARGS( &surface.resource ) ), "CreateCommittedResource (rt history)" ) ) {
			return false;
		}
		surface.resource->SetName( NAMES[static_cast<size_t>( slot )] );
		surface.srv = srvPool.Allocate();
		srvPool.CreateSrv( surface.srv, *surface.resource.Get(), srv );
		surface.uav = srvPool.Allocate();
		srvPool.CreateUav( surface.uav, *surface.resource.Get(), uav );
		bytes += static_cast<uint64>( pair.width ) * pair.height * BytesPerTexel( pair.format );
	}
	return true;
}
