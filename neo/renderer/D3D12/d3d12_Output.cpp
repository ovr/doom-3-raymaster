// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Output.h"

namespace {

constexpr DXGI_FORMAT COLOR_RESOURCE = DXGI_FORMAT_R8G8B8A8_TYPELESS;
constexpr DXGI_FORMAT COLOR_UNORM = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT DEPTH_RESOURCE = DXGI_FORMAT_R24G8_TYPELESS;
constexpr DXGI_FORMAT DEPTH_SHADER_VIEW = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
// the values the clears of a frame use most, so the driver can clear them fast
constexpr float CLEAR_COLOR[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
constexpr float CLEAR_DEPTH = 1.0f;
constexpr UINT8 CLEAR_STENCIL = STENCIL_SHADOW_TEST_VALUE;

D3D12_RESOURCE_DESC TextureDesc( DXGI_FORMAT format, int width, int height, UINT samples, D3D12_RESOURCE_FLAGS flags ) {
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = static_cast<UINT64>( width );
	desc.Height = static_cast<UINT>( height );
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = samples;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	return desc;
}

[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC ShaderView( DXGI_FORMAT format, UINT samples ) {
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = format;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if ( samples > 1 ) {
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	} else {
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipLevels = 1;
	}
	return desc;
}

constexpr uint32 AlignUp( uint32 value, uint32 align ) {
	return ( value + align - 1 ) & ~( align - 1 );
}

}	// namespace

UINT D3D12_SceneSamples( int requested, std::span<const bool, 4> supported ) {
	for ( size_t level = supported.size() - 1; level >= 1; level-- ) {
		const int samples = 1 << level;
		if ( samples <= requested && supported[level] ) {
			return static_cast<UINT>( samples );
		}
	}
	return 1;
}

idD3D12SceneTarget::idD3D12SceneTarget( const idD3D12Device & owner, idD3D12Graveyard & graves, idD3D12DescriptorPool & srvs,
	idD3D12DescriptorPool & rtvs, idD3D12DescriptorPool & dsvs ) :
	device( owner ),
	graveyard( graves ),
	srvPool( srvs ),
	rtvPool( rtvs ),
	dsvPool( dsvs ) {
	rtvUnorm = rtvPool.Allocate();
	rtvSrgb = rtvPool.Allocate();
	rtvHdr = rtvPool.Allocate();
	rtvLights = rtvPool.Allocate();
	for ( d3d12Descriptor_t & rtv : rtvGBuffer ) {
		rtv = rtvPool.Allocate();
	}
	dsv = dsvPool.Allocate();
}

// The RTVs and the DSV are rewritten in their slots, a recorded list keeps what it was given. A submitted list
// reads the SRVs from the shader-visible heap when it runs: each new resource takes new SRV slots.
void idD3D12SceneTarget::Create( int newWidth, int newHeight, UINT sampleCount ) {
	const auto bury = [this]( surface_t & surface ) {
		graveyard.Bury( std::move( surface.resource ) );
		if ( surface.srv.IsValid() ) {
			graveyard.Bury( srvPool, surface.srv );
			surface.srv = {};
		}
		surface.state = {};
	};
	for ( surface_t * surface : { &color, &depth, &resolved, &hdrColor, &hdrResolved, &lights } ) {
		bury( *surface );
	}
	std::ranges::for_each( gbuffer, bury );
	width = Max( newWidth, 1 );
	height = Max( newHeight, 1 );
	samples = sampleCount;

	CreateColor( color, COLOR_RESOURCE, samples, true, L"doom3 scene colour" );
	for ( const bool srgb : { false, true } ) {
		D3D12_RENDER_TARGET_VIEW_DESC view = {};
		view.Format = Target( srgb ).ColorView();
		view.ViewDimension = ( samples > 1 ) ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
		device.Get().CreateRenderTargetView( color.resource.Get(), &view, Rtv( srgb ) );
	}
	if ( samples > 1 ) {
		CreateColor( resolved, COLOR_RESOURCE, 1, false, L"doom3 scene resolve" );
	}
	CreateDepth();
}

// Its contents are radiance times PreExposure; r_clear 0 never clears it, so it starts black.
void idD3D12SceneTarget::EnsureHdr( idD3D12Barriers & barriers, idD3D12Submitter & submitter ) {
	if ( hdrColor.resource ) {
		return;
	}
	CreateColor( hdrColor, d3d12TargetId_t::HDR_FORMAT, samples, true, L"doom3 HDR scene colour" );
	if ( !hdrColor.resource ) {
		return;
	}
	D3D12_RENDER_TARGET_VIEW_DESC view = {};
	view.Format = d3d12TargetId_t::HDR_FORMAT;
	view.ViewDimension = ( samples > 1 ) ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
	device.Get().CreateRenderTargetView( hdrColor.resource.Get(), &view, Rtv( false, true ) );
	if ( samples > 1 ) {
		CreateColor( hdrResolved, d3d12TargetId_t::HDR_FORMAT, 1, false, L"doom3 HDR scene resolve" );
	}
	barriers.Transition( *hdrColor.resource.Get(), hdrColor.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	barriers.Flush();
	submitter.List().ClearRenderTargetView( Rtv( false, true ), CLEAR_COLOR, 0, nullptr );
}

// Not cleared here: BeginGBuffer clears the viewport it draws.
bool idD3D12SceneTarget::EnsureGBuffer() {
	static constexpr const wchar_t * NAMES[] = { L"doom3 G-buffer normal", L"doom3 G-buffer F0 roughness", L"doom3 G-buffer motion",
		L"doom3 G-buffer albedo" };
	static_assert( std::size( NAMES ) == GBUFFER_TARGETS );
	for ( size_t i = 0; i < GBUFFER_TARGETS; i++ ) {
		if ( gbuffer[i].resource ) {
			continue;
		}
		const DXGI_FORMAT format = d3d12TargetId_t::GBUFFER_FORMATS[i];
		CreateColor( gbuffer[i], format, samples, true, NAMES[i] );
		if ( !gbuffer[i].resource ) {
			return false;
		}
		D3D12_RENDER_TARGET_VIEW_DESC view = {};
		view.Format = format;
		view.ViewDimension = ( samples > 1 ) ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
		device.Get().CreateRenderTargetView( gbuffer[i].resource.Get(), &view, rtvPool.Cpu( rtvGBuffer[i] ) );
	}
	return true;
}

std::array<D3D12_CPU_DESCRIPTOR_HANDLE, idD3D12SceneTarget::GBUFFER_TARGETS> idD3D12SceneTarget::GBufferRtvs() const {
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, GBUFFER_TARGETS> rtvs;
	for ( size_t i = 0; i < GBUFFER_TARGETS; i++ ) {
		rtvs[i] = rtvPool.Cpu( rtvGBuffer[i] );
	}
	return rtvs;
}

void idD3D12SceneTarget::ClearGBuffer( idD3D12Barriers & barriers, ID3D12GraphicsCommandList4 & list, const D3D12_RECT & rect ) {
	for ( surface_t & target : gbuffer ) {
		barriers.Transition( *target.resource.Get(), target.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	}
	barriers.Flush();
	for ( const D3D12_CPU_DESCRIPTOR_HANDLE rtv : GBufferRtvs() ) {
		list.ClearRenderTargetView( rtv, CLEAR_COLOR, 1, &rect );
	}
}

// Not cleared here: BeginTracedLights clears the rectangle it draws.
bool idD3D12SceneTarget::EnsureLights() {
	if ( lights.resource ) {
		return true;
	}
	CreateColor( lights, d3d12TargetId_t::HDR_FORMAT, samples, true, L"doom3 light target" );
	if ( !lights.resource ) {
		return false;
	}
	D3D12_RENDER_TARGET_VIEW_DESC view = {};
	view.Format = d3d12TargetId_t::HDR_FORMAT;
	view.ViewDimension = ( samples > 1 ) ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
	device.Get().CreateRenderTargetView( lights.resource.Get(), &view, rtvPool.Cpu( rtvLights ) );
	return true;
}

void idD3D12SceneTarget::ClearLights( idD3D12Barriers & barriers, ID3D12GraphicsCommandList4 & list, const D3D12_RECT & rect ) {
	barriers.Transition( *lights.resource.Get(), lights.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	barriers.Flush();
	list.ClearRenderTargetView( rtvPool.Cpu( rtvLights ), CLEAR_COLOR, 1, &rect );
}

void idD3D12SceneTarget::CreateColor( surface_t & surface, DXGI_FORMAT format, UINT sampleCount, bool renderTarget, const wchar_t * name ) {
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_RESOURCE_DESC desc = TextureDesc( format, width, height, sampleCount,
		renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE );
	const DXGI_FORMAT viewFormat = ( format == COLOR_RESOURCE ) ? COLOR_UNORM : format;
	D3D12_CLEAR_VALUE clear = { viewFormat };
	memcpy( clear.Color, CLEAR_COLOR, sizeof( clear.Color ) );
	surface.state.current = renderTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_RESOLVE_DEST;
	if ( device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, surface.state.current,
			renderTarget ? &clear : nullptr, IID_PPV_ARGS( &surface.resource ) ), "CreateCommittedResource (scene colour)" ) ) {
		surface.resource->SetName( name );
		surface.srv = srvPool.Allocate();
		srvPool.CreateSrv( surface.srv, *surface.resource.Get(), ShaderView( viewFormat, sampleCount ) );
	}
}

void idD3D12SceneTarget::CreateDepth() {
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_RESOURCE_DESC desc = TextureDesc( DEPTH_RESOURCE, width, height, samples, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL );
	D3D12_CLEAR_VALUE clear = { d3d12TargetId_t::DEPTH };
	clear.DepthStencil = { CLEAR_DEPTH, CLEAR_STENCIL };
	depth.state.current = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	if ( !device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, depth.state.current, &clear,
			IID_PPV_ARGS( &depth.resource ) ), "CreateCommittedResource (scene depth)" ) ) {
		return;
	}
	depth.resource->SetName( L"doom3 scene depth" );
	D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
	view.Format = d3d12TargetId_t::DEPTH;
	view.ViewDimension = ( samples > 1 ) ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
	device.Get().CreateDepthStencilView( depth.resource.Get(), &view, Dsv() );
	depth.srv = srvPool.Allocate();
	srvPool.CreateSrv( depth.srv, *depth.resource.Get(), ShaderView( DEPTH_SHADER_VIEW, samples ) );
}

idD3D12SceneTarget::surface_t & idD3D12SceneTarget::Resolved( idD3D12Barriers & barriers, idD3D12Submitter & submitter,
	std::optional<D3D12_RECT> region, bool hdr ) {
	surface_t & source = Color( hdr );
	if ( samples == 1 ) {
		return source;
	}
	surface_t & target = hdr ? hdrResolved : resolved;
	const DXGI_FORMAT format = hdr ? d3d12TargetId_t::HDR_FORMAT : COLOR_UNORM;
	barriers.Transition( *source.resource.Get(), source.state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE );
	barriers.Transition( *target.resource.Get(), target.state, D3D12_RESOURCE_STATE_RESOLVE_DEST );
	barriers.Flush();
	if ( region ) {
		assert( region->left >= 0 && region->top >= 0 && region->right <= width && region->bottom <= height );
		submitter.List().ResolveSubresourceRegion( target.resource.Get(), 0, static_cast<UINT>( region->left ), static_cast<UINT>( region->top ),
			source.resource.Get(), 0, &*region, format, D3D12_RESOLVE_MODE_AVERAGE );
	} else {
		submitter.List().ResolveSubresource( target.resource.Get(), 0, source.resource.Get(), 0, format );
	}
	return target;
}

// The engine owns full screen (a borderless window, win_window.cpp) and Alt+Enter (the wndproc), so
// DXGI is told to leave the window alone and SetFullscreenState is never called.
std::expected<idD3D12SwapChain, idStr> idD3D12SwapChain::Create( const idD3D12Device & owner, idD3D12Queue & queue,
	idD3D12DescriptorPool & rtvs, HWND window, int clientWidth, int clientHeight ) {
	idD3D12SwapChain created;
	created.device = &owner;
	created.rtvPool = &rtvs;
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
	for ( backBuffer_t & buffer : created.buffers ) {
		buffer.rtv = rtvs.Allocate();
	}
	created.AcquireBuffers();
	return created;
}

void idD3D12SwapChain::AcquireBuffers() {
	for ( UINT i = 0; i < BUFFERS; i++ ) {
		backBuffer_t & buffer = buffers[i];
		buffer.state = {};
		if ( device->Check( swapChain->GetBuffer( i, IID_PPV_ARGS( &buffer.resource ) ), "IDXGISwapChain::GetBuffer" ) ) {
			buffer.resource->SetName( L"doom3 back buffer" );
			device->Get().CreateRenderTargetView( buffer.resource.Get(), nullptr, rtvPool->Cpu( buffer.rtv ) );
		}
	}
}

// DXGI has no adaptive vsync: ADAPTIVE presents like VSYNC.
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

idD3D12Readback::idD3D12Readback( const idD3D12Device & owner, idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12Graveyard & graves ) :
	device( owner ),
	submitter( submit ),
	barriers( batch ),
	graveyard( graves ) {
}

// The part of the request outside the source reads black.
void idD3D12Readback::Read( ID3D12Resource & source, d3d12ResourceState_t & state, int x, int y, int width, int height, byte * rgb, bool flipRows ) {
	memset( rgb, 0, static_cast<size_t>( width ) * height * 3 );
	const D3D12_RESOURCE_DESC desc = source.GetDesc();
	const int sourceHeight = static_cast<int>( desc.Height );
	const int x0 = Max( x, 0 );
	const int y0 = Max( y, 0 );
	const int x1 = Min( x + width, static_cast<int>( desc.Width ) );
	const int y1 = Min( y + height, sourceHeight );
	if ( x1 <= x0 || y1 <= y0 ) {
		return;
	}
	const UINT copyWidth = static_cast<UINT>( x1 - x0 );
	const UINT copyHeight = static_cast<UINT>( y1 - y0 );
	const UINT rowPitch = AlignUp( copyWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT );
	const uint64 bytes = static_cast<uint64>( rowPitch ) * copyHeight;
	if ( bytes > size ) {
		graveyard.Bury( std::move( buffer ) );
		buffer = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, L"doom3 readback" );
		size = buffer ? bytes : 0;
	}
	if ( !buffer ) {
		return;
	}

	// a back buffer counts its rows from the top of the screen
	const UINT top = static_cast<UINT>( flipRows ? sourceHeight - y1 : y0 );
	barriers.Transition( source, state, D3D12_RESOURCE_STATE_COPY_SOURCE );
	barriers.Flush();
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = buffer.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Footprint = { desc.Format, copyWidth, copyHeight, 1, rowPitch };
	D3D12_TEXTURE_COPY_LOCATION from = {};
	from.pResource = &source;
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const D3D12_BOX box = { static_cast<UINT>( x0 ), top, 0, static_cast<UINT>( x1 ), top + copyHeight, 1 };
	submitter.List().CopyTextureRegion( &destination, 0, 0, 0, &from, &box );
	submitter.WaitIdle();

	const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>( bytes ) };
	void * data = nullptr;
	if ( !device.Check( buffer->Map( 0, &readRange, &data ), "Map (readback)" ) ) {
		return;
	}
	const byte * mapped = static_cast<const byte *>( data );
	for ( UINT row = 0; row < copyHeight; row++ ) {
		const int glRow = flipRows ? y1 - 1 - static_cast<int>( row ) : y0 + static_cast<int>( row );
		const byte * in = mapped + static_cast<size_t>( row ) * rowPitch;
		byte * out = rgb + ( static_cast<size_t>( glRow - y ) * width + ( x0 - x ) ) * 3;
		for ( UINT column = 0; column < copyWidth; column++, in += 4, out += 3 ) {
			out[0] = in[0];
			out[1] = in[1];
			out[2] = in[2];
		}
	}
	const D3D12_RANGE nothingWritten = { 0, 0 };
	buffer->Unmap( 0, &nothingWritten );
}
