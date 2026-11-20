// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Dxc.h"

#include <algorithm>
#include <string>

namespace {

// names, entry points, profiles and defines are ASCII
std::wstring Widen( std::string_view text ) {
	return std::wstring( text.begin(), text.end() );
}

}	// namespace

std::expected<d3d12DxcOutput_t, idStr> D3D12_DxcCompile( const idD3D12Library & library, std::string_view source, const char * name,
	const char * entry, const char * profile, std::span<const char * const> defines ) {
	// a compiler per call: IDxcCompiler3 is not safe across threads
	idComPtr<IDxcCompiler3> compiler;
	HRESULT hr = library.dxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( &compiler ) );
	if ( FAILED( hr ) ) {
		return std::unexpected( idStr( va( "DxcCreateInstance (IDxcCompiler3) failed: %s", D3D12_ErrorText( hr ).c_str() ) ) );
	}

	std::vector<std::wstring> args = { Widen( name ), L"-E", Widen( entry ), L"-T", Widen( profile ), L"-HV", L"2021", L"-O3" };
	for ( const char * define : defines ) {
		args.push_back( L"-D" );
		args.push_back( Widen( define ) );
	}
	std::vector<LPCWSTR> argv;
	for ( const std::wstring & arg : args ) {
		argv.push_back( arg.c_str() );
	}
	const DxcBuffer buffer = { source.data(), source.size(), DXC_CP_UTF8 };
	idComPtr<IDxcResult> result;
	hr = compiler->Compile( &buffer, argv.data(), static_cast<UINT32>( argv.size() ), nullptr, IID_PPV_ARGS( &result ) );
	if ( FAILED( hr ) ) {
		return std::unexpected( idStr( va( "IDxcCompiler3::Compile( %s ) failed: %s", name, D3D12_ErrorText( hr ).c_str() ) ) );
	}

	idComPtr<IDxcBlobUtf8> messages;
	result->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS( &messages ), nullptr );
	idStr text;
	if ( messages && messages->GetStringLength() > 0 ) {
		text = messages->GetStringPointer();
	}
	HRESULT status = E_FAIL;
	result->GetStatus( &status );
	if ( FAILED( status ) ) {
		return std::unexpected( text.IsEmpty() ? idStr( va( "%s: DXC failed with %s", name, D3D12_ErrorText( status ).c_str() ) ) : text );
	}
	idComPtr<IDxcBlob> object;
	result->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS( &object ), nullptr );
	if ( !object || object->GetBufferSize() == 0 ) {
		return std::unexpected( idStr( va( "%s: DXC returned no DXIL", name ) ) );
	}
	d3d12DxcOutput_t output;
	const byte * code = static_cast<const byte *>( object->GetBufferPointer() );
	output.code.assign( code, code + object->GetBufferSize() );
	output.warnings = text;
	return output;
}

bool D3D12_DxilSigned( std::span<const byte> container ) {
	constexpr size_t DIGEST_OFFSET = 4;
	constexpr size_t DIGEST_SIZE = 16;
	if ( container.size() < DIGEST_OFFSET + DIGEST_SIZE || memcmp( container.data(), "DXBC", 4 ) != 0 ) {
		return false;
	}
	const std::span<const byte> digest = container.subspan( DIGEST_OFFSET, DIGEST_SIZE );
	return std::any_of( digest.begin(), digest.end(), []( byte b ) { return b != 0; } );
}
