// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Dxc.h"

#include <algorithm>
#include <optional>
#include <string>

namespace {

// va's buffers are shared, and D3D12_DxcCompile runs on several threads at once.
template <typename... Args>
[[nodiscard]] idStr Text( const char * format, Args... args ) {
	idStr text;
	text.Format( format, args... );
	return text;
}

// names, entry points, profiles and defines are ASCII
std::wstring Widen( std::string_view text ) {
	return std::wstring( text.begin(), text.end() );
}

// DXC asks for ".\shaders\include\common.hlsli" or ".\shaders\internal\..\include\bindless.hlsli": the path
// under base/ with forward slashes and without "." or "..", which the file system refuses.
idStr IncludePath( LPCWSTR fileName ) {
	std::vector<std::string> parts( 1 );
	for ( const wchar_t * c = fileName; *c != 0; c++ ) {
		if ( *c == L'\\' || *c == L'/' ) {
			parts.emplace_back();
		} else {
			parts.back() += static_cast<char>( *c );
		}
	}
	std::vector<std::string> kept;
	for ( const std::string & part : parts ) {
		if ( part == ".." && !kept.empty() ) {
			kept.pop_back();
		} else if ( !part.empty() && part != "." ) {
			kept.push_back( part );
		}
	}
	idStr path;
	for ( const std::string & part : kept ) {
		if ( !path.IsEmpty() ) {
			path += "/";
		}
		path += part.c_str();
	}
	return path;
}

// Lives on the stack of one call: DXC holds it only during Compile.
class idDxcIncludeHandler final : public IDxcIncludeHandler {
public:
	idDxcIncludeHandler( IDxcUtils & dxcUtils, const d3d12ShaderReader_t & read ) : utils( dxcUtils ), reader( read ) {}
	virtual ~idDxcIncludeHandler() = default;
	HRESULT STDMETHODCALLTYPE LoadSource( LPCWSTR fileName, IDxcBlob ** includeSource ) override {
		*includeSource = nullptr;
		const std::optional<idStr> text = reader( IncludePath( fileName ) );
		if ( !text ) {
			return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
		}
		idComPtr<IDxcBlobEncoding> blob;
		const HRESULT hr = utils.CreateBlob( text->c_str(), static_cast<UINT32>( text->Length() ), DXC_CP_UTF8, &blob );
		if ( SUCCEEDED( hr ) ) {
			*includeSource = blob.Detach();
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void ** object ) override {
		if ( riid == __uuidof( IDxcIncludeHandler ) || riid == __uuidof( IUnknown ) ) {
			*object = static_cast<IDxcIncludeHandler *>( this );
			return S_OK;
		}
		*object = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }

private:
	IDxcUtils &						utils;
	const d3d12ShaderReader_t &		reader;
};

std::expected<idComPtr<IDxcUtils>, idStr> CreateUtils( const idD3D12Library & library ) {
	idComPtr<IDxcUtils> utils;
	const HRESULT hr = library.dxcCreateInstance( CLSID_DxcUtils, IID_PPV_ARGS( &utils ) );
	if ( FAILED( hr ) ) {
		return std::unexpected( Text( "DxcCreateInstance (IDxcUtils) failed: %s", D3D12_ErrorText( hr ).c_str() ) );
	}
	return utils;
}

std::vector<std::wstring> Arguments( const char * name, std::span<const char * const> defines ) {
	std::vector<std::wstring> args = { Widen( name ), L"-HV", L"2021" };
	for ( const char * define : defines ) {
		args.push_back( L"-D" );
		args.push_back( Widen( define ) );
	}
	return args;
}

// The result of a successful run and its warnings; the errors otherwise.
std::expected<idComPtr<IDxcResult>, idStr> Run( const idD3D12Library & library, std::string_view source, const char * name,
	std::span<const std::wstring> args, const d3d12ShaderReader_t * include, idStr & warnings ) {
	// a compiler per call: IDxcCompiler3 is not safe across threads
	idComPtr<IDxcCompiler3> compiler;
	HRESULT hr = library.dxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( &compiler ) );
	if ( FAILED( hr ) ) {
		return std::unexpected( Text( "DxcCreateInstance (IDxcCompiler3) failed: %s", D3D12_ErrorText( hr ).c_str() ) );
	}
	idComPtr<IDxcUtils> utils;
	std::optional<idDxcIncludeHandler> handler;
	if ( include != nullptr ) {
		auto created = CreateUtils( library );
		if ( !created ) {
			return std::unexpected( created.error() );
		}
		utils = std::move( *created );
		handler.emplace( *utils.Get(), *include );
	}
	std::vector<LPCWSTR> argv;
	for ( const std::wstring & arg : args ) {
		argv.push_back( arg.c_str() );
	}
	const DxcBuffer buffer = { source.data(), source.size(), DXC_CP_UTF8 };
	idComPtr<IDxcResult> result;
	hr = compiler->Compile( &buffer, argv.data(), static_cast<UINT32>( argv.size() ), handler ? &*handler : nullptr, IID_PPV_ARGS( &result ) );
	if ( FAILED( hr ) ) {
		return std::unexpected( Text( "IDxcCompiler3::Compile( %s ) failed: %s", name, D3D12_ErrorText( hr ).c_str() ) );
	}
	idComPtr<IDxcBlobUtf8> errors;
	result->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS( &errors ), nullptr );
	idStr messages;
	if ( errors && errors->GetStringLength() > 0 ) {
		messages = errors->GetStringPointer();
		messages.StripTrailingWhitespace();
	}
	HRESULT status = E_FAIL;
	result->GetStatus( &status );
	if ( FAILED( status ) ) {
		return std::unexpected( messages.IsEmpty() ? Text( "%s: DXC failed with %s", name, D3D12_ErrorText( status ).c_str() ) : messages );
	}
	warnings = messages;
	return result;
}

}	// namespace

std::expected<d3d12DxcOutput_t, idStr> D3D12_DxcCompile( const idD3D12Library & library, std::string_view source, const char * name,
	const char * entry, const char * profile, std::span<const char * const> defines, const d3d12ShaderReader_t * include ) {
	std::vector<std::wstring> args = Arguments( name, defines );
	args.insert( args.end(), { L"-E", Widen( entry ), L"-T", Widen( profile ), L"-O3" } );
	idStr warnings;
	const auto result = Run( library, source, name, args, include, warnings );
	if ( !result ) {
		return std::unexpected( result.error() );
	}
	idComPtr<IDxcBlob> object;
	( *result )->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS( &object ), nullptr );
	if ( !object || object->GetBufferSize() == 0 ) {
		return std::unexpected( Text( "%s: DXC returned no DXIL", name ) );
	}
	d3d12DxcOutput_t output;
	const byte * code = static_cast<const byte *>( object->GetBufferPointer() );
	output.code.assign( code, code + object->GetBufferSize() );
	output.warnings = warnings;
	return output;
}

std::expected<idStr, idStr> D3D12_DxcPreprocess( const idD3D12Library & library, std::string_view source, const char * name,
	std::span<const char * const> defines, const d3d12ShaderReader_t & include, const char * profile ) {
	std::vector<std::wstring> args = Arguments( name, defines );
	if ( profile != nullptr ) {
		args.insert( args.end(), { L"-T", Widen( profile ) } );
	}
	args.push_back( L"-P" );
	idStr warnings;
	const auto result = Run( library, source, name, args, &include, warnings );
	if ( !result ) {
		return std::unexpected( result.error() );
	}
	idComPtr<IDxcBlobUtf8> text;
	( *result )->GetOutput( DXC_OUT_HLSL, IID_PPV_ARGS( &text ), nullptr );
	if ( !text ) {
		return std::unexpected( Text( "%s: DXC returned no preprocessed text", name ) );
	}
	idStr preprocessed;
	preprocessed.Append( text->GetStringPointer(), static_cast<int>( text->GetStringLength() ) );
	return preprocessed;
}

std::expected<idComPtr<ID3D12ShaderReflection>, idStr> D3D12_DxcReflect( const idD3D12Library & library, std::span<const byte> container ) {
	auto utils = CreateUtils( library );
	if ( !utils ) {
		return std::unexpected( utils.error() );
	}
	const DxcBuffer buffer = { container.data(), container.size(), 0 };
	idComPtr<ID3D12ShaderReflection> reflection;
	const HRESULT hr = ( *utils )->CreateReflection( &buffer, IID_PPV_ARGS( &reflection ) );
	if ( FAILED( hr ) ) {
		return std::unexpected( Text( "IDxcUtils::CreateReflection failed: %s", D3D12_ErrorText( hr ).c_str() ) );
	}
	return reflection;
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
