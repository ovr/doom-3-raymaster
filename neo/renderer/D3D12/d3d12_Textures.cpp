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

#include <bit>
#include <string>

#include "../tr_local.h"
// the codec's parameters shadow its members (C4458), and this folder builds with /WX
#pragma warning( push )
#pragma warning( disable : 4458 )
#include "../DXT/DXTCodec.h"
#pragma warning( pop )
#include "d3d12_StateMap.h"
#include "d3d12_Textures.h"

namespace {

[[nodiscard]] constexpr int Blocks4( int pixels ) {
	return ( pixels + 3 ) / 4;
}

[[nodiscard]] constexpr uint64 AlignUp( uint64 value, uint64 align ) {
	return ( value + align - 1 ) & ~( align - 1 );
}

[[nodiscard]] constexpr int MaxLevels( int width, int height ) {
	int levels = 1;
	for ( int size = std::max( width, height ); size > 1; size >>= 1 ) {
		levels++;
	}
	return levels;
}

[[nodiscard]] std::wstring WideName( const char * prefix, const char * name ) {
	std::wstring wide;
	for ( const char * c : { prefix, name } ) {
		for ( ; *c != '\0'; c++ ) {
			wide.push_back( static_cast<wchar_t>( static_cast<unsigned char>( *c ) ) );
		}
	}
	return wide;
}

// GL uploads RGB565 with UNPACK_SWAP_BYTES: the words of the image files are big-endian
void SwapRow565( const byte * from, byte * to, int width ) {
	for ( int x = 0; x < width; x++ ) {
		to[x * 2] = from[x * 2 + 1];
		to[x * 2 + 1] = from[x * 2];
	}
}

[[nodiscard]] const char * FormatName( textureFormat_t format ) {
	constexpr const char * NAMES[] = { "NONE", "RGBA8", "XRGB8", "ALPHA", "L8A8", "LUM8", "INT8", "DXT1", "DXT5", "DEPTH", "X16", "Y16_X16", "RGB565" };
	return ( format >= 0 && format < static_cast<int>( std::size( NAMES ) ) ) ? NAMES[format] : "?";
}

}	// namespace

uint64 D3D12_HashBytes( uint64 hash, std::span<const byte> bytes ) {
	constexpr uint64 K = 0x9FB21C651E98DF25ull;
	size_t i = 0;
	for ( ; i + 8 <= bytes.size(); i += 8 ) {
		uint64 word;
		memcpy( &word, bytes.data() + i, 8 );
		hash = std::rotl( ( hash ^ word ) * K, 29 );
	}
	uint64 tail = static_cast<uint64>( bytes.size() ) << 56;
	memcpy( &tail, bytes.data() + i, bytes.size() - i );
	return std::rotl( ( hash ^ tail ) * K, 29 );
}

idD3D12TextureStore::idD3D12TextureStore( const idD3D12Device & owner, idD3D12Graveyard & graves, idD3D12DescriptorPool & srvs,
	idD3D12DescriptorPool & rtvs, idD3D12Uploader & copies, idD3D12Barriers & batch, bool unaligned ) :
	device( owner ),
	graveyard( graves ),
	srvPool( srvs ),
	rtvPool( rtvs ),
	uploader( copies ),
	barriers( batch ),
	unalignedBlocks( unaligned ) {
}

// Never uploaded again, so they go to PIXEL_SHADER_RESOURCE for good.
void idD3D12TextureStore::CreateDefaults() {
	const d3d12FormatInfo_t format = D3D12_FormatFor( FMT_RGBA8, CFM_DEFAULT, textureRole_t::SAMPLED, 1, 1, true );
	constexpr byte BLACK[4] = { 0, 0, 0, 255 };
	for ( const textureKind_t kind : { textureKind_t::TEX_2D, textureKind_t::TEX_CUBE } ) {
		d3d12TextureSurface_t & surface = fallbacks[static_cast<size_t>( kind )];
		if ( !CreateSurface( surface, format, kind, 1, 1, 1, ( kind == textureKind_t::TEX_2D ) ? "_d3d12Black2D" : "_d3d12BlackCube" ) ) {
			continue;
		}
		for ( int face = 0; face < surface.faces; face++ ) {
			static_cast<void>( CopyRegion( surface, format, static_cast<UINT>( face ), true, 1, 1, BLACK, sizeof( BLACK ), false ) );
		}
		barriers.Transition( *surface.resource.Get(), surface.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	}
}

bool idD3D12TextureStore::CreateSurface( d3d12TextureSurface_t & surface, const d3d12FormatInfo_t & format, textureKind_t kind, int width,
	int height, int levels, const char * name ) {
	const int faces = ( kind == textureKind_t::TEX_CUBE ) ? 6 : 1;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = static_cast<UINT64>( width );
	desc.Height = static_cast<UINT>( height );
	desc.DepthOrArraySize = static_cast<UINT16>( faces );
	desc.MipLevels = static_cast<UINT16>( levels );
	desc.Format = format.resource;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = ( format.rtv != DXGI_FORMAT_UNKNOWN ) ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	if ( !device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS( &surface.resource ) ), "CreateCommittedResource (texture)" ) ) {
		return false;
	}
	surface.resource->SetName( WideName( "doom3 texture ", name ).c_str() );
	surface.state = {};
	surface.state.current = D3D12_RESOURCE_STATE_COPY_DEST;
	surface.width = width;
	surface.height = height;
	surface.levels = levels;
	surface.faces = faces;
	surface.bytes = device.Get().GetResourceAllocationInfo( 0, 1, &desc ).SizeInBytes;

	D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = format.srv;
	view.Shader4ComponentMapping = format.mapping;
	if ( kind == textureKind_t::TEX_CUBE ) {
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		view.TextureCube.MipLevels = static_cast<UINT>( levels );
	} else {
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Texture2D.MipLevels = static_cast<UINT>( levels );
	}
	surface.srv = srvPool.Allocate();
	device.Get().CreateShaderResourceView( surface.resource.Get(), &view, srvPool.Cpu( surface.srv ) );
	if ( format.rtv != DXGI_FORMAT_UNKNOWN ) {
		D3D12_RENDER_TARGET_VIEW_DESC target = {};
		target.Format = format.rtv;
		target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		surface.rtv = rtvPool.Allocate();
		device.Get().CreateRenderTargetView( surface.resource.Get(), &target, rtvPool.Cpu( surface.rtv ) );
	}
	return true;
}

textureHandle_t idD3D12TextureStore::Create( const idImageOpts & opts, textureRole_t role, const samplerDesc_t & sampler, const char * name ) {
	const char * debugName = ( name != nullptr ) ? name : "";
	const textureKind_t kind = ( opts.textureType == TT_CUBIC ) ? textureKind_t::TEX_CUBE : textureKind_t::TEX_2D;
	const int width = Max( 1, opts.width );
	const int height = ( kind == textureKind_t::TEX_CUBE ) ? width : Max( 1, opts.height );
	const d3d12FormatInfo_t format = D3D12_FormatFor( opts.format, opts.colorFormat, role, width, height, unalignedBlocks );
	if ( format.resource == DXGI_FORMAT_UNKNOWN ) {
		common->Warning( "d3d12: texture '%s' has no format (%d)", debugName, static_cast<int>( opts.format ) );
		return textureHandle_t::NONE;
	}
	d3d12Texture_t texture;
	texture.opts = opts;
	texture.role = role;
	texture.kind = kind;
	texture.format = format;
	texture.sampler = D3D12_PackSampler( sampler );
	texture.name = debugName;
	const int levels = idMath::ClampInt( 1, MaxLevels( width, height ), opts.numLevels );
	if ( !CreateSurface( texture.surface, format, kind, width, height, levels, debugName ) ) {
		return textureHandle_t::NONE;
	}
	bytes += texture.surface.bytes;
	const textureHandle_t handle = table.Add( std::move( texture ) );
	if ( handle == textureHandle_t::NONE ) {
		common->Warning( "d3d12: no room for texture '%s' in the handle table", debugName );
		BurySurface( texture.surface );
	}
	return handle;
}

void idD3D12TextureStore::BurySurface( d3d12TextureSurface_t & surface ) {
	bytes -= surface.bytes;
	graveyard.Bury( std::move( surface.resource ) );
	graveyard.Bury( srvPool, surface.srv );
	graveyard.Bury( rtvPool, surface.rtv );
	surface = {};
}

// A batched barrier points at the state inside the record, so it goes into the list before the record moves.
void idD3D12TextureStore::Destroy( textureHandle_t handle ) {
	const d3d12Texture_t * texture = table.Find( handle );
	if ( texture == nullptr ) {
		return;
	}
	if ( texture->surface.state.pending >= 0 || texture->spare.state.pending >= 0 ) {
		barriers.Flush();
	}
	d3d12Texture_t dead = table.Remove( handle );
	BurySurface( dead.surface );
	BurySurface( dead.spare );
}

// Swapping surfaces moves their states, which a batched barrier points at: the batch goes into the list first.
d3d12TextureSurface_t * idD3D12TextureStore::PrepareCopyTarget( textureHandle_t handle, d3d12CopyKind_t copy, int width, int height ) {
	d3d12Texture_t * texture = table.Find( handle );
	if ( texture == nullptr || width <= 0 || height <= 0 ) {
		return nullptr;
	}
	const bool fits = ( copy == d3d12CopyKind_t::COLOR ) ? ( texture->format.resource == DXGI_FORMAT_R8G8B8A8_UNORM )
		: ( texture->format.rtv == DXGI_FORMAT_R32_FLOAT );
	if ( texture->kind != textureKind_t::TEX_2D || !fits ) {
		common->Warning( "d3d12: '%s' is no target of a %s copy", texture->name.c_str(), ( copy == d3d12CopyKind_t::COLOR ) ? "colour" : "depth" );
		return nullptr;
	}
	if ( texture->surface.width != width || texture->surface.height != height ) {
		if ( texture->surface.state.pending >= 0 || texture->spare.state.pending >= 0 ) {
			barriers.Flush();
		}
		if ( texture->spare.resource && texture->spare.width == width && texture->spare.height == height ) {
			std::swap( texture->surface, texture->spare );
		} else {
			d3d12TextureSurface_t created;
			if ( !CreateSurface( created, texture->format, textureKind_t::TEX_2D, width, height, 1, texture->name.c_str() ) ) {
				return nullptr;
			}
			bytes += created.bytes;
			copySurfaces++;
			BurySurface( texture->spare );
			texture->spare = std::move( texture->surface );
			texture->surface = std::move( created );
		}
	}
	texture->copiedResized = ( width != Max( 1, texture->opts.width ) || height != Max( 1, texture->opts.height ) );
	texture->uploads.clear();	// level 0 holds no upload now, and the surface may be another one
	if ( copy == d3d12CopyKind_t::COLOR ) {
		samplerDesc_t sampler = D3D12_UnpackSampler( texture->sampler );
		sampler.filter = samplerFilter_t::LINEAR;
		sampler.address = samplerAddress_t::CLAMP;
		texture->sampler = D3D12_PackSampler( sampler );
	}
	return &texture->surface;
}

// DXT levels come padded to whole blocks, a cube level under 4 pixels says 4: the copy takes the level's
// own size. A level the resource lacks (numLevels beyond the size) is dropped.
void idD3D12TextureStore::Upload( textureHandle_t handle, int mipLevel, int face, int width, int height, const void * pixels, int pixelPitch ) {
	d3d12Texture_t * texture = table.Find( handle );
	if ( texture == nullptr || pixels == nullptr || width <= 0 || height <= 0 ) {
		return;
	}
	d3d12TextureSurface_t & surface = texture->surface;
	if ( mipLevel < 0 || mipLevel >= surface.levels || face < 0 || face >= surface.faces ) {
		return;
	}
	const d3d12FormatInfo_t & format = texture->format;
	const int levelWidth = Max( 1, surface.width >> mipLevel );
	const int levelHeight = Max( 1, surface.height >> mipLevel );
	const int regionWidth = Min( width, levelWidth );
	const int regionHeight = Min( height, levelHeight );

	const byte * source = static_cast<const byte *>( pixels );
	size_t sourcePitch = static_cast<size_t>( ( pixelPitch > 0 ) ? pixelPitch : width ) * format.sourceBytes;
	if ( format.sourceBlocks ) {
		sourcePitch = static_cast<size_t>( Blocks4( width ) ) * format.sourceBytes;
	}
	if ( format.convert == d3d12Convert_t::DECODE_DXT1 || format.convert == d3d12Convert_t::DECODE_DXT5 ) {
		// the decoder steps whole blocks and strides by its width
		const int decodedWidth = Blocks4( width ) * 4;
		const int decodedHeight = Blocks4( height ) * 4;
		scratch.resize( static_cast<size_t>( decodedWidth ) * decodedHeight * 4 );
		idDxtDecoder decoder;
		if ( format.convert == d3d12Convert_t::DECODE_DXT1 ) {
			decoder.DecompressImageDXT1( source, scratch.data(), decodedWidth, decodedHeight );
		} else {
			decoder.DecompressImageDXT5( source, scratch.data(), decodedWidth, decodedHeight );
		}
		source = scratch.data();
		sourcePitch = static_cast<size_t>( decodedWidth ) * 4;
	}

	if ( recordUploads && texture->uploads.empty() ) {
		texture->uploads.resize( static_cast<size_t>( surface.levels ) * surface.faces );
	}
	const UINT subresource = static_cast<UINT>( mipLevel + face * surface.levels );
	const bool wholeLevel = ( regionWidth == levelWidth && regionHeight == levelHeight );
	const d3d12UploadRecord_t record = CopyRegion( surface, format, subresource, wholeLevel, regionWidth, regionHeight, source, sourcePitch,
		recordUploads );
	if ( recordUploads ) {
		texture->uploads[subresource] = record;
	}
}

// A BC level is copied with the footprint the device gives it (its rows of whole blocks); everything else
// with a footprint of the region. Converted rows go straight into the write-combined staging unless hashed.
d3d12UploadRecord_t idD3D12TextureStore::CopyRegion( d3d12TextureSurface_t & surface, const d3d12FormatInfo_t & format, UINT subresource,
	bool wholeLevel, int width, int height, const byte * source, size_t sourcePitch, bool record ) {
	const size_t rowBytes = static_cast<size_t>( format.compressed ? Blocks4( width ) : width ) * format.blockBytes;
	D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT rows = 0;
	if ( format.compressed && wholeLevel ) {
		const D3D12_RESOURCE_DESC desc = surface.resource->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = {};
		device.Get().GetCopyableFootprints( &desc, subresource, 1, 0, &placed, &rows, nullptr, nullptr );
		footprint = placed.Footprint;
	} else {
		footprint.Format = format.resource;
		footprint.Width = static_cast<UINT>( format.compressed ? Blocks4( width ) * 4 : width );
		footprint.Height = static_cast<UINT>( format.compressed ? Blocks4( height ) * 4 : height );
		footprint.Depth = 1;
		footprint.RowPitch = static_cast<UINT>( AlignUp( rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ) );
		rows = static_cast<UINT>( format.compressed ? Blocks4( height ) : height );
	}
	const std::optional<d3d12UploadSpan_t> span = uploader.Stage( static_cast<uint64>( footprint.RowPitch ) * rows,
		D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT );
	if ( !span ) {
		return {};
	}

	d3d12UploadRecord_t result;
	result.rowBytes = static_cast<uint32>( rowBytes );
	result.rows = rows;
	result.uploaded = record;
	const bool convertRows = ( format.convert == d3d12Convert_t::SWAP_565 );
	if ( convertRows && record ) {
		rowScratch.resize( rowBytes );
	}
	for ( UINT row = 0; row < rows; row++ ) {
		const byte * from = source + row * sourcePitch;
		byte * to = span->cpu + static_cast<size_t>( row ) * footprint.RowPitch;
		if ( !convertRows ) {
			memcpy( to, from, rowBytes );
		} else if ( !record ) {
			SwapRow565( from, to, width );
		} else {
			SwapRow565( from, rowScratch.data(), width );
			memcpy( to, rowScratch.data(), rowBytes );
			from = rowScratch.data();
		}
		if ( record ) {
			result.hash = D3D12_HashBytes( result.hash, std::span( from, rowBytes ) );
		}
	}
	uploader.TextureRegion( *surface.resource.Get(), surface.state, subresource, *span, footprint, rows );
	return result;
}

void idD3D12TextureStore::SetSampler( textureHandle_t handle, const samplerDesc_t & sampler ) {
	if ( d3d12Texture_t * texture = table.Find( handle ) ) {
		texture->sampler = D3D12_PackSampler( sampler );
	}
}

textureHandle_t idD3D12TextureStore::FindByName( const char * name ) {
	textureHandle_t found = textureHandle_t::NONE;
	table.ForEach( [&]( textureHandle_t handle, const d3d12Texture_t & texture ) {
		if ( found == textureHandle_t::NONE && texture.name.Icmp( name ) == 0 ) {
			found = handle;
		}
	} );
	return found;
}

d3d12Descriptor_t idD3D12TextureStore::SampledView( textureHandle_t handle, textureKind_t kind ) {
	const d3d12Texture_t * texture = table.Find( handle );
	return ( texture != nullptr && texture->kind == kind && !texture->IncompleteCopy() ) ? texture->surface.srv : Fallback( kind );
}

// GL_TEXTURE_MAX_LEVEL is numLevels - 1, and glCopyTexImage2D redefines level 0 only.
bool d3d12Texture_t::IncompleteCopy() const {
	if ( !copiedResized || opts.numLevels <= 1 ) {
		return false;
	}
	const samplerFilter_t filter = D3D12_UnpackSampler( sampler ).filter;
	return filter == samplerFilter_t::LINEAR_MIP_NEAREST || filter == samplerFilter_t::LINEAR_MIP_LINEAR;
}

void idD3D12TextureStore::MakeReadable( textureHandle_t handle ) {
	if ( d3d12Texture_t * texture = table.Find( handle ) ) {
		barriers.Transition( *texture->surface.resource.Get(), texture->surface.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	}
}

// Shared code destroys its images before the backend; the fallbacks and the rest die here.
void idD3D12TextureStore::DestroyAll() {
	if ( table.Num() > 0 ) {
		common->DPrintf( "d3d12: %d textures still alive at shutdown\n", table.Num() );
	}
	table.Clear();
	fallbacks = {};
	bytes = 0;
}

idD3D12TextureCheck::idD3D12TextureCheck( const idD3D12Device & owner, idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12Graveyard & graves ) :
	device( owner ),
	submitter( submit ),
	barriers( batch ),
	graveyard( graves ) {
}

d3d12TextureCheckResult_t idD3D12TextureCheck::Run( idD3D12TextureStore & store ) {
	d3d12TextureCheckResult_t result;
	used = 0;
	pending.clear();
	store.ForEach( [&]( textureHandle_t, d3d12Texture_t & texture ) {
		result.textures++;
		bool recorded = false;
		for ( UINT subresource = 0; subresource < texture.uploads.size(); subresource++ ) {
			const d3d12UploadRecord_t & record = texture.uploads[subresource];
			if ( !record.uploaded || device.IsLost() ) {
				continue;
			}
			recorded = true;
			const D3D12_RESOURCE_DESC desc = texture.surface.resource->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = {};
			UINT64 total = 0;
			device.Get().GetCopyableFootprints( &desc, subresource, 1, 0, &placed, nullptr, nullptr, &total );
			uint64 offset = AlignUp( used, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT );
			if ( offset + total > size ) {
				if ( !pending.empty() ) {
					Compare( result );
				}
				offset = 0;
			}
			if ( total > size ) {
				graveyard.Bury( std::move( buffer ) );
				size = Max( BATCH, total );
				buffer = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST, L"doom3 texture check readback" );
			}
			if ( !buffer ) {
				size = 0;
				continue;
			}
			placed.Offset = offset;
			barriers.Transition( *texture.surface.resource.Get(), texture.surface.state, D3D12_RESOURCE_STATE_COPY_SOURCE );
			barriers.Flush();
			D3D12_TEXTURE_COPY_LOCATION destination = {};
			destination.pResource = buffer.Get();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			destination.PlacedFootprint = placed;
			D3D12_TEXTURE_COPY_LOCATION source = {};
			source.pResource = texture.surface.resource.Get();
			source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			source.SubresourceIndex = subresource;
			submitter.List().CopyTextureRegion( &destination, 0, 0, 0, &source, nullptr );
			pending.push_back( { &texture, subresource, offset, placed.Footprint.RowPitch, record } );
			used = offset + total;
			result.subresources++;
			result.bytes += total;
		}
		result.unrecorded += recorded ? 0 : 1;
	} );
	if ( !pending.empty() ) {
		Compare( result );
	}
	graveyard.Bury( std::move( buffer ) );
	size = 0;
	common->Printf( "d3d12 checkImages: %d textures (%d without recorded uploads), %d subresources, %.1f MB read back: %d mismatches\n",
		result.textures, result.unrecorded, result.subresources, static_cast<double>( result.bytes ) / ( 1024.0 * 1024.0 ), result.mismatches );
	return result;
}

void idD3D12TextureCheck::Compare( d3d12TextureCheckResult_t & result ) {
	submitter.WaitIdle();
	const D3D12_RANGE everything = { 0, static_cast<SIZE_T>( used ) };
	void * mapped = nullptr;
	if ( !buffer || !device.Check( buffer->Map( 0, &everything, &mapped ), "Map (texture check readback)" ) ) {
		pending.clear();
		used = 0;
		return;
	}
	for ( const pending_t & read : pending ) {
		const byte * rows = static_cast<const byte *>( mapped ) + read.offset;
		uint64 hash = D3D12_HASH_SEED;
		for ( uint32 row = 0; row < read.expected.rows; row++ ) {
			hash = D3D12_HashBytes( hash, std::span( rows + static_cast<size_t>( row ) * read.rowPitch, read.expected.rowBytes ) );
		}
		if ( hash != read.expected.hash ) {
			const d3d12TextureSurface_t & surface = read.texture->surface;
			result.mismatches++;
			common->Printf( "d3d12 checkImages: mismatch in '%s' level %u face %u (%s, %d x %d, %u levels)\n", read.texture->name.c_str(),
				read.subresource % static_cast<UINT>( surface.levels ), read.subresource / static_cast<UINT>( surface.levels ),
				FormatName( read.texture->opts.format ), surface.width, surface.height, static_cast<unsigned>( surface.levels ) );
		}
	}
	const D3D12_RANGE nothingWritten = { 0, 0 };
	buffer->Unmap( 0, &nothingWritten );
	pending.clear();
	used = 0;
}
