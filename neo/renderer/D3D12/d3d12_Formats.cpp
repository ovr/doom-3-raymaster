// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Formats.h"

namespace {

enum mapFrom_t : UINT {
	R = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
	G = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
	B = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
	A = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3,
	ONE = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
};

[[nodiscard]] constexpr UINT Map( mapFrom_t r, mapFrom_t g, mapFrom_t b, mapFrom_t a ) {
	return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING( r, g, b, a );
}

[[nodiscard]] constexpr d3d12FormatInfo_t Plain( DXGI_FORMAT format, UINT mapping, uint8 bytes ) {
	d3d12FormatInfo_t info;
	info.resource = format;
	info.srv = format;
	info.mapping = mapping;
	info.blockBytes = bytes;
	info.sourceBytes = bytes;
	return info;
}

[[nodiscard]] constexpr d3d12FormatInfo_t Depth() {
	d3d12FormatInfo_t info = Plain( DXGI_FORMAT_R32_FLOAT, Map( R, R, R, ONE ), 4 );
	info.rtv = DXGI_FORMAT_R32_FLOAT;
	return info;
}

[[nodiscard]] constexpr d3d12FormatInfo_t Blocks( DXGI_FORMAT format, uint8 bytes, d3d12Convert_t decode, bool decoded ) {
	d3d12FormatInfo_t info = decoded ? Plain( DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4 )
		: Plain( format, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, bytes );
	info.sourceBytes = bytes;
	info.compressed = !decoded;
	info.sourceBlocks = true;
	info.convert = decoded ? decode : d3d12Convert_t::NONE;
	return info;
}

}	// namespace

d3d12FormatInfo_t D3D12_FormatFor( textureFormat_t format, textureColor_t color, textureRole_t role, int width, int height, bool unalignedBlocks ) {
	if ( role == textureRole_t::DEPTH_COPY ) {
		return Depth();
	}
	if ( role == textureRole_t::COLOR_COPY ) {
		return Plain( DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4 );
	}
	const bool decoded = !unalignedBlocks && ( ( width & 3 ) != 0 || ( height & 3 ) != 0 );
	d3d12FormatInfo_t info;
	switch ( format ) {
		case FMT_RGBA8:		info = Plain( DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4 ); break;
		case FMT_XRGB8:		info = Plain( DXGI_FORMAT_R8G8B8A8_UNORM, Map( R, G, B, ONE ), 4 ); break;
		case FMT_ALPHA:		info = Plain( DXGI_FORMAT_R8_UNORM, Map( ONE, ONE, ONE, R ), 1 ); break;
		case FMT_L8A8:		info = Plain( DXGI_FORMAT_R8G8_UNORM, Map( R, R, R, G ), 2 ); break;
		case FMT_LUM8:		info = Plain( DXGI_FORMAT_R8_UNORM, Map( R, R, R, ONE ), 1 ); break;
		case FMT_INT8:		info = Plain( DXGI_FORMAT_R8_UNORM, Map( R, R, R, R ), 1 ); break;
		case FMT_DXT1:		info = Blocks( DXGI_FORMAT_BC1_UNORM, 8, d3d12Convert_t::DECODE_DXT1, decoded ); break;
		case FMT_DXT5:		info = Blocks( DXGI_FORMAT_BC3_UNORM, 16, d3d12Convert_t::DECODE_DXT5, decoded ); break;
		case FMT_DEPTH:		info = Depth(); break;
		case FMT_X16:		info = Plain( DXGI_FORMAT_R16_UNORM, Map( R, R, R, R ), 2 ); break;
		case FMT_Y16_X16:	info = Plain( DXGI_FORMAT_R16G16_UNORM, Map( R, R, R, G ), 4 ); break;
		case FMT_RGB565:
			info = Plain( DXGI_FORMAT_B5G6R5_UNORM, Map( R, G, B, ONE ), 2 );
			info.convert = d3d12Convert_t::SWAP_565;
			break;
		default:
			return {};
	}
	if ( color == CFM_GREEN_ALPHA ) {
		info.mapping = Map( ONE, ONE, ONE, G );
	}
	return info;
}
