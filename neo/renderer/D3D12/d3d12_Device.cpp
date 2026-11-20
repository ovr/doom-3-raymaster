// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Descriptors.h"
#include "d3d12_Timers.h"

#include <filesystem>

namespace {

// The messages the backend causes on purpose: clears with a colour or depth other than the clear value.
constexpr D3D12_MESSAGE_ID MUTED_MESSAGES[] = {
	D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
	D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
};

constexpr const char * SEVERITY_NAMES[] = { "CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE" };

const char * SeverityName( int severity ) {
	return ( severity >= 0 && severity < static_cast<int>( std::size( SEVERITY_NAMES ) ) ) ? SEVERITY_NAMES[severity] : "?";
}

idStr Utf8( const wchar_t * text ) {
	char buffer[512] = {};
	WideCharToMultiByte( CP_UTF8, 0, text, -1, buffer, static_cast<int>( sizeof( buffer ) ) - 1, nullptr, nullptr );
	return idStr( buffer );
}

template<typename F>
bool Resolve( HMODULE module, const char * name, F & function ) {
	function = reinterpret_cast<F>( reinterpret_cast<void *>( GetProcAddress( module, name ) ) );
	return function != nullptr;
}

// VS_FIXEDFILEINFO straight from the module's resources: no version.dll, which the engine does not link.
uint64 FileVersion( HMODULE module ) {
	HRSRC info = FindResourceA( module, MAKEINTRESOURCEA( VS_VERSION_INFO ), MAKEINTRESOURCEA( 16 ) );	// RT_VERSION
	HGLOBAL loaded = ( info != nullptr ) ? LoadResource( module, info ) : nullptr;
	const byte * data = ( loaded != nullptr ) ? static_cast<const byte *>( LockResource( loaded ) ) : nullptr;
	if ( data == nullptr ) {
		return 0;
	}
	const DWORD size = SizeofResource( module, info );
	for ( DWORD offset = 0; offset + sizeof( VS_FIXEDFILEINFO ) <= size; offset += 4 ) {
		VS_FIXEDFILEINFO fixed;
		memcpy( &fixed, data + offset, sizeof( fixed ) );
		if ( fixed.dwSignature == VS_FFI_SIGNATURE ) {
			return ( static_cast<uint64>( fixed.dwFileVersionMS ) << 32 ) | fixed.dwFileVersionLS;
		}
	}
	return 0;
}

idStr VersionText( uint64 version ) {
	return va( "%u.%u.%u.%u", static_cast<unsigned>( version >> 48 ), static_cast<unsigned>( ( version >> 32 ) & 0xFFFF ),
		static_cast<unsigned>( ( version >> 16 ) & 0xFFFF ), static_cast<unsigned>( version & 0xFFFF ) );
}

const char * VendorName( uint32 vendorId ) {
	switch ( vendorId ) {
		case 0x1002:	return "AMD";
		case 0x10DE:	return "NVIDIA";
		case 0x8086:	return "Intel";
		case 0x1414:	return "Microsoft";
		default:		return "unknown vendor";
	}
}

// DX12 Ultimate. Bindless (ResourceDescriptorHeap) needs shader model 6.6 and resource binding tier 3.
constexpr D3D_FEATURE_LEVEL REQUIRED_LEVEL = D3D_FEATURE_LEVEL_12_2;
constexpr D3D_SHADER_MODEL REQUIRED_SHADER_MODEL = D3D_SHADER_MODEL_6_6;
constexpr const char * REQUIREMENT = "DX12 Ultimate, feature level 12_2 and shader model 6.6 (Radeon RX 6000, GeForce RTX 20, Intel Arc or newer)";
constexpr const char * OPENGL_HINT = "+set r_renderApi opengl starts the old renderer";

idStr LevelText( D3D_FEATURE_LEVEL level ) {
	return va( "%d_%d", ( level >> 12 ) & 0xF, ( level >> 8 ) & 0xF );
}

idStr ShaderModelText( D3D_SHADER_MODEL model ) {
	return va( "%d.%d", ( model >> 4 ) & 0xF, model & 0xF );
}

idStr AdapterName( IDXGIAdapter1 & adapter ) {
	DXGI_ADAPTER_DESC1 desc = {};
	adapter.GetDesc1( &desc );
	return Utf8( desc.Description );
}

// The highest feature level the adapter has; 0 when it has no Direct3D 12 at all.
int MaxFeatureLevel( const idD3D12Library & library, IDXGIAdapter1 & adapter ) {
	idComPtr<ID3D12Device> device;
	if ( FAILED( library.createDevice( &adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS( &device ) ) ) ) {
		return 0;
	}
	constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
		D3D_FEATURE_LEVEL_12_2 };
	D3D12_FEATURE_DATA_FEATURE_LEVELS data = { static_cast<UINT>( std::size( levels ) ), levels, D3D_FEATURE_LEVEL_11_0 };
	return D3D12_CheckFeature( *device.Get(), D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof( data ) ) ? data.MaxSupportedFeatureLevel : D3D_FEATURE_LEVEL_11_0;
}

// "AMD Radeon RX 5700 has feature level 12_1", for the error the player sees.
idStr LevelOf( const idD3D12Library & library, IDXGIAdapter1 & adapter ) {
	const int level = MaxFeatureLevel( library, adapter );
	return ( level == 0 ) ? va( "%s has no Direct3D 12", AdapterName( adapter ).c_str() )
		: va( "%s has feature level %s", AdapterName( adapter ).c_str(), LevelText( static_cast<D3D_FEATURE_LEVEL>( level ) ).c_str() );
}

bool CanCreateDevice( const idD3D12Library & library, IDXGIAdapter1 & adapter ) {
	return SUCCEEDED( library.createDevice( &adapter, REQUIRED_LEVEL, __uuidof( ID3D12Device ), nullptr ) );
}

// Which D3D12Core.dll the device runs on: the Agility SDK's from D3D12\ next to the exe, or the system's.
idStr RuntimeText() {
	HMODULE core = GetModuleHandleW( L"D3D12Core.dll" );
	wchar_t corePath[MAX_PATH] = {};
	wchar_t exePath[MAX_PATH] = {};
	if ( core == nullptr || GetModuleFileNameW( core, corePath, MAX_PATH ) == 0 || GetModuleFileNameW( nullptr, exePath, MAX_PATH ) == 0 ) {
		return "system";
	}
	const std::filesystem::path agilityDir = std::filesystem::path( exePath ).parent_path() / L"D3D12";
	std::error_code failed;
	const bool agility = std::filesystem::equivalent( std::filesystem::path( corePath ).parent_path(), agilityDir, failed );
	return va( "%s %s", agility ? "Agility SDK" : "system", VersionText( FileVersion( core ) ).c_str() );
}

// "" picks the first hardware adapter by GPU preference that can run D3D12, "warp" the software one, "N" the Nth.
std::expected<idComPtr<IDXGIAdapter1>, idStr> FindAdapter( const idD3D12Library & library, IDXGIFactory6 & factory, const char * name ) {
	idComPtr<IDXGIAdapter1> adapter;
	if ( idStr::Icmp( name, "warp" ) == 0 ) {
		if ( FAILED( factory.EnumWarpAdapter( IID_PPV_ARGS( &adapter ) ) ) ) {
			return std::unexpected( idStr( "no WARP adapter" ) );
		}
		return adapter;
	}
	if ( name[0] != '\0' ) {
		if ( !idStr::IsNumeric( name ) || atoi( name ) < 0 ) {
			return std::unexpected( idStr( va( "r_d3d12Adapter \"%s\": not \"\", \"warp\" or an adapter index", name ) ) );
		}
		const UINT index = static_cast<UINT>( atoi( name ) );
		if ( FAILED( factory.EnumAdapterByGpuPreference( index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS( &adapter ) ) ) ) {
			return std::unexpected( idStr( va( "no adapter %u", index ) ) );
		}
		return adapter;
	}
	idStr rejected;
	for ( UINT i = 0; SUCCEEDED( factory.EnumAdapterByGpuPreference( i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS( &adapter ) ) ); i++ ) {
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1( &desc );
		if ( ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) == 0 ) {
			if ( CanCreateDevice( library, *adapter.Get() ) ) {
				return adapter;
			}
			rejected += va( "%s%s", rejected.IsEmpty() ? "" : ", ", LevelOf( library, *adapter.Get() ).c_str() );
		}
		adapter.Reset();
	}
	return std::unexpected( idStr( va( "Direct3D 12 needs %s; %s. %s", REQUIREMENT, rejected.IsEmpty() ? "there is no GPU" : rejected.c_str(),
		OPENGL_HINT ) ) );
}

// Before the device exists: the layer, GPU-based validation and DRED are process settings read at creation.
int EnableDebugLayers( const idD3D12Library & library, int debugLevel ) {
	if ( debugLevel <= 0 ) {
		return 0;
	}
	idComPtr<ID3D12Debug> debug;
	if ( FAILED( library.getDebugInterface( IID_PPV_ARGS( &debug ) ) ) ) {
		common->Warning( "d3d12: r_debugContext %d needs the debug layer (D3D12\\d3d12SDKLayers.dll next to the exe, or the Windows "
			"optional feature Graphics Tools)", debugLevel );
		return 0;
	}
	debug->EnableDebugLayer();
	if ( debugLevel >= 2 ) {
		idComPtr<ID3D12Debug1> debug1;
		if ( SUCCEEDED( debug.As( &debug1 ) ) ) {
			debug1->SetEnableGPUBasedValidation( TRUE );
		}
	}
	if ( debugLevel >= 3 ) {
		idComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
		if ( SUCCEEDED( library.getDebugInterface( IID_PPV_ARGS( &dred ) ) ) ) {
			dred->SetAutoBreadcrumbsEnablement( D3D12_DRED_ENABLEMENT_FORCED_ON );
			dred->SetPageFaultEnablement( D3D12_DRED_ENABLEMENT_FORCED_ON );
		}
	}
	return debugLevel;
}

template<typename T>
bool Feature( ID3D12Device2 & device, D3D12_FEATURE feature, T & data ) {
	return D3D12_CheckFeature( device, feature, &data, sizeof( data ) );
}

// D3D12_AUTO_BREADCRUMB_OP up to WRITEBUFFERIMMEDIATE: every operation this backend records.
constexpr const char * BREADCRUMB_OPS[] = { "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
	"ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
	"ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present",
	"ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint",
	"AtomicCopyBufferUint64", "ResolveSubresourceRegion", "WriteBufferImmediate" };

idStr BreadcrumbOp( D3D12_AUTO_BREADCRUMB_OP op ) {
	const size_t index = static_cast<size_t>( op );
	return ( index < std::size( BREADCRUMB_OPS ) ) ? idStr( BREADCRUMB_OPS[index] ) : idStr( va( "operation %d", static_cast<int>( op ) ) );
}

const char * OrUnnamed( const char * name ) {
	return ( name != nullptr && name[0] != '\0' ) ? name : "(unnamed)";
}

void PrintBreadcrumb( const D3D12_AUTO_BREADCRUMB_NODE & node ) {
	common->Printf( "d3d12 dred: %s\n", D3D12_BreadcrumbText( node ).c_str() );
}

const char * DredDeviceState( D3D12_DRED_DEVICE_STATE state ) {
	switch ( state ) {
		case D3D12_DRED_DEVICE_STATE_HUNG:		return "hung";
		case D3D12_DRED_DEVICE_STATE_FAULT:		return "faulted";
		case D3D12_DRED_DEVICE_STATE_PAGEFAULT:	return "page fault";
		default:								return "unknown (removed by the application or the driver)";
	}
}

void PrintAllocations( const char * what, const D3D12_DRED_ALLOCATION_NODE * node ) {
	constexpr int MAX_NAMES = 8;
	for ( int i = 0; node != nullptr && i < MAX_NAMES; node = node->pNext, i++ ) {
		common->Printf( "d3d12 dred: %s allocation %s (type %d)\n", what, OrUnnamed( node->ObjectNameA ), static_cast<int>( node->AllocationType ) );
	}
}

}	// namespace

bool D3D12_CheckFeature( ID3D12Device & device, D3D12_FEATURE feature, void * data, UINT size ) {
	idComPtr<ID3D12Device16> device16;
	if ( SUCCEEDED( device.QueryInterface( IID_PPV_ARGS( &device16 ) ) ) ) {
		return SUCCEEDED( device16->CheckFeatureSupport1( feature, data, size, D3D12_SDK_VERSION ) );
	}
	return SUCCEEDED( device.CheckFeatureSupport( feature, data, size ) );
}

idStr D3D12_BreadcrumbText( const D3D12_AUTO_BREADCRUMB_NODE & node ) {
	const UINT done = ( node.pLastBreadcrumbValue != nullptr ) ? *node.pLastBreadcrumbValue : 0;
	idStr where;
	if ( done < node.BreadcrumbCount ) {
		where = va( ", stopped before %s", BreadcrumbOp( node.pCommandHistory[done] ).c_str() );
	} else if ( node.BreadcrumbCount > 0 ) {
		where = va( ", the last %s", BreadcrumbOp( node.pCommandHistory[node.BreadcrumbCount - 1] ).c_str() );
	}
	return va( "list %s on %s: %u of %u operations done%s", OrUnnamed( node.pCommandListDebugNameA ), OrUnnamed( node.pCommandQueueDebugNameA ),
		done, node.BreadcrumbCount, where.c_str() );
}

idStr D3D12_ErrorText( HRESULT hr ) {
	const char * name = "";
	switch ( hr ) {
		case E_FAIL:							name = " E_FAIL"; break;
		case E_INVALIDARG:						name = " E_INVALIDARG"; break;
		case E_OUTOFMEMORY:						name = " E_OUTOFMEMORY"; break;
		case E_NOTIMPL:							name = " E_NOTIMPL"; break;
		case E_NOINTERFACE:						name = " E_NOINTERFACE"; break;
		case DXGI_ERROR_DEVICE_REMOVED:			name = " DXGI_ERROR_DEVICE_REMOVED"; break;
		case DXGI_ERROR_DEVICE_RESET:			name = " DXGI_ERROR_DEVICE_RESET"; break;
		case DXGI_ERROR_DEVICE_HUNG:			name = " DXGI_ERROR_DEVICE_HUNG"; break;
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR:	name = " DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
		case DXGI_ERROR_INVALID_CALL:			name = " DXGI_ERROR_INVALID_CALL"; break;
		case DXGI_ERROR_UNSUPPORTED:			name = " DXGI_ERROR_UNSUPPORTED"; break;
		case DXGI_ERROR_NOT_FOUND:				name = " DXGI_ERROR_NOT_FOUND"; break;
		case D3D12_ERROR_ADAPTER_NOT_FOUND:		name = " D3D12_ERROR_ADAPTER_NOT_FOUND"; break;
		case D3D12_ERROR_DRIVER_VERSION_MISMATCH:	name = " D3D12_ERROR_DRIVER_VERSION_MISMATCH"; break;
		default:								break;
	}
	return va( "0x%08X%s", static_cast<unsigned>( hr ), name );
}

std::expected<idD3D12Library, idStr> idD3D12Library::Load() {
	idD3D12Library library;
	library.d3d12.reset( LoadLibraryExW( L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 ) );
	library.dxgi.reset( LoadLibraryExW( L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 ) );
	if ( !library.d3d12 || !library.dxgi ) {
		return std::unexpected( idStr( va( "%s is missing from System32", !library.d3d12 ? "d3d12.dll" : "dxgi.dll" ) ) );
	}

	const char * missing = nullptr;
	if ( !Resolve( library.d3d12.get(), "D3D12CreateDevice", library.createDevice ) ) {
		missing = "D3D12CreateDevice";
	} else if ( !Resolve( library.d3d12.get(), "D3D12GetDebugInterface", library.getDebugInterface ) ) {
		missing = "D3D12GetDebugInterface";
	} else if ( !Resolve( library.d3d12.get(), "D3D12SerializeVersionedRootSignature", library.serializeRootSignature ) ) {
		missing = "D3D12SerializeVersionedRootSignature";
	} else if ( !Resolve( library.dxgi.get(), "CreateDXGIFactory2", library.createFactory2 ) ) {
		missing = "CreateDXGIFactory2";
	} else if ( !Resolve( library.dxgi.get(), "DXGIGetDebugInterface1", library.getDxgiDebug ) ) {
		missing = "DXGIGetDebugInterface1";
	}
	if ( missing != nullptr ) {
		return std::unexpected( idStr( va( "the system DLLs have no %s", missing ) ) );
	}

	// dxil.dll first: dxcompiler.dll loads it by name and then gets this one, never another on the search path
	library.dxil.reset( LoadLibraryExW( L"dxil.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR ) );
	library.dxc.reset( LoadLibraryExW( L"dxcompiler.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR ) );
	if ( !library.dxil || !library.dxc ) {
		return std::unexpected( idStr( va( "%s is missing next to the exe (the build copies it from external/d3d12-x64)",
			!library.dxil ? "dxil.dll" : "dxcompiler.dll" ) ) );
	}
	if ( !Resolve( library.dxc.get(), "DxcCreateInstance", library.dxcCreateInstance ) ) {
		return std::unexpected( idStr( "dxcompiler.dll has no DxcCreateInstance" ) );
	}
	library.dxcVersion = FileVersion( library.dxc.get() );
	return library;
}

std::expected<idD3D12Device, idStr> idD3D12Device::Create( const idD3D12Library & library, const char * adapterName, int requestedDebugLevel ) {
	idD3D12Device created;
	created.debugLevel = EnableDebugLayers( library, requestedDebugLevel );

	// the DXGI debug layer is a separate part of Graphics Tools: without it the factory is made plain
	HRESULT hr = E_FAIL;
	if ( created.debugLevel > 0 ) {
		hr = library.createFactory2( DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS( &created.factory ) );
	}
	if ( FAILED( hr ) ) {
		hr = library.createFactory2( 0, IID_PPV_ARGS( &created.factory ) );
	}
	if ( FAILED( hr ) ) {
		return std::unexpected( idStr( va( "CreateDXGIFactory2 (IDXGIFactory6) failed: %s", D3D12_ErrorText( hr ).c_str() ) ) );
	}

	auto found = FindAdapter( library, *created.factory.Get(), adapterName );
	if ( !found ) {
		return std::unexpected( found.error() );
	}
	created.adapter = std::move( *found );

	hr = library.createDevice( created.adapter.Get(), REQUIRED_LEVEL, IID_PPV_ARGS( &created.device ) );
	if ( FAILED( hr ) ) {
		if ( MaxFeatureLevel( library, *created.adapter.Get() ) < REQUIRED_LEVEL ) {
			const bool warp = idStr::Icmp( adapterName, "warp" ) == 0;
			return std::unexpected( idStr( va( "Direct3D 12 needs %s; %s%s. %s", REQUIREMENT, LevelOf( library, *created.adapter.Get() ).c_str(),
				warp ? " (the system's WARP: d3d10warp.dll of the WARP package next to the exe has 12_2)" : "", OPENGL_HINT ) ) );
		}
		return std::unexpected( idStr( va( "D3D12CreateDevice (ID3D12Device2, feature level 12_2) failed: %s", D3D12_ErrorText( hr ).c_str() ) ) );
	}
	created.device->SetName( L"doom3 device" );
	created.ReadFeatures();
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	const bool bindingTier3 = Feature( created.Get(), D3D12_FEATURE_D3D12_OPTIONS, options )
		&& options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
	if ( created.features.shaderModel < REQUIRED_SHADER_MODEL || !bindingTier3 ) {
		return std::unexpected( idStr( va( "Direct3D 12 needs %s; %s has shader model %s%s (a newer driver may have it). %s", REQUIREMENT,
			created.features.adapterName.c_str(), ShaderModelText( created.features.shaderModel ).c_str(),
			bindingTier3 ? "" : " and resource binding below tier 3", OPENGL_HINT ) ) );
	}

	if ( created.debugLevel > 0 && SUCCEEDED( created.device.As( &created.infoQueue ) ) ) {
		constexpr D3D12_MESSAGE_SEVERITY hidden[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
		D3D12_INFO_QUEUE_FILTER filter = {};
		filter.DenyList.NumSeverities = static_cast<UINT>( std::size( hidden ) );
		filter.DenyList.pSeverityList = const_cast<D3D12_MESSAGE_SEVERITY *>( hidden );
		filter.DenyList.NumIDs = static_cast<UINT>( std::size( MUTED_MESSAGES ) );
		filter.DenyList.pIDList = const_cast<D3D12_MESSAGE_ID *>( MUTED_MESSAGES );
		created.infoQueue->PushStorageFilter( &filter );
		// the layer breaks on corruption by default, which kills a game without a debugger before the log has it
		created.infoQueue->SetBreakOnSeverity( D3D12_MESSAGE_SEVERITY_CORRUPTION, IsDebuggerPresent() );
		created.infoQueue->SetBreakOnSeverity( D3D12_MESSAGE_SEVERITY_ERROR, IsDebuggerPresent() );
	}
	if ( created.debugLevel > 0 && SUCCEEDED( library.getDxgiDebug( 0, IID_PPV_ARGS( &created.dxgiInfoQueue ) ) ) ) {
		constexpr DXGI_INFO_QUEUE_MESSAGE_SEVERITY hidden[] = { DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_MESSAGE };
		DXGI_INFO_QUEUE_FILTER filter = {};
		filter.DenyList.NumSeverities = static_cast<UINT>( std::size( hidden ) );
		filter.DenyList.pSeverityList = const_cast<DXGI_INFO_QUEUE_MESSAGE_SEVERITY *>( hidden );
		created.dxgiInfoQueue->PushStorageFilter( DXGI_DEBUG_ALL, &filter );
		created.dxgiInfoQueue->SetBreakOnSeverity( DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, IsDebuggerPresent() );
		created.dxgiInfoQueue->SetBreakOnSeverity( DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, IsDebuggerPresent() );
	}
	return created;
}

void idD3D12Device::ReadFeatures() {
	DXGI_ADAPTER_DESC1 desc = {};
	adapter->GetDesc1( &desc );
	features.adapterName = Utf8( desc.Description );
	features.vendorId = desc.VendorId;
	features.dedicatedVideoMemory = desc.DedicatedVideoMemory;
	features.warp = ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) != 0;
	LARGE_INTEGER umdVersion = {};
	if ( SUCCEEDED( adapter->CheckInterfaceSupport( __uuidof( IDXGIDevice ), &umdVersion ) ) ) {
		features.driverVersion = static_cast<uint64>( umdVersion.QuadPart );
	}
	ID3D12Device2 & d3d = *device.Get();
	// a runtime older than the headers rejects a model it does not know: ask again one lower
	D3D12_FEATURE_DATA_SHADER_MODEL model = { D3D_HIGHEST_SHADER_MODEL };
	while ( !Feature( d3d, D3D12_FEATURE_SHADER_MODEL, model ) && model.HighestShaderModel > D3D_SHADER_MODEL_6_0 ) {
		model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>( model.HighestShaderModel - 1 );
	}
	features.shaderModel = model.HighestShaderModel;
	features.runtime = RuntimeText();
	features.versionString = va( "Direct3D 12, feature level 12_2, shader model %s, %s runtime, driver %s",
		ShaderModelText( features.shaderModel ).c_str(), features.runtime.c_str(), VersionText( features.driverVersion ).c_str() );

	for ( size_t i = 0; i < features.samples.size(); i++ ) {
		bool supported = true;
		for ( DXGI_FORMAT format : { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT } ) {
			D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
			levels.Format = format;
			levels.SampleCount = 1u << i;
			supported = supported && Feature( d3d, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, levels ) && levels.NumQualityLevels > 0;
		}
		features.samples[i] = supported;
	}
	// WARP (10.0.26100) reports it, but a PSO with DepthBoundsTestEnable passes every depth test there,
	// even with bounds ( 0, 1 ): the world draws through walls (test RasterDepthOrder).
	D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2 = {};
	features.depthBounds = Feature( d3d, D3D12_FEATURE_D3D12_OPTIONS2, options2 ) && options2.DepthBoundsTestSupported && !features.warp;
	D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8 = {};
	features.unalignedBlocks = Feature( d3d, D3D12_FEATURE_D3D12_OPTIONS8, options8 ) && options8.UnalignedBlockTexturesSupported;
	D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
	if ( Feature( d3d, D3D12_FEATURE_D3D12_OPTIONS16, options16 ) ) {
		features.gpuUploadHeap = options16.GPUUploadHeapSupported;
		features.dynamicDepthBias = options16.DynamicDepthBiasSupported;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS19 options19 = {};
	features.anisoPointMip = Feature( d3d, D3D12_FEATURE_D3D12_OPTIONS19, options19 ) && options19.AnisoFilterWithPointMipSupported;
	BOOL tearing = FALSE;
	features.tearing = SUCCEEDED( factory->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof( tearing ) ) ) && tearing;
}

bool idD3D12Device::Check( HRESULT hr, const char * what ) const {
	if ( SUCCEEDED( hr ) ) {
		return true;
	}
	if ( hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ) {
		lost = true;
		ReportLoss();
		return false;
	}
	DrainMessages();	// the layer usually says why
	common->FatalError( "d3d12: %s failed: %s", what, D3D12_ErrorText( hr ).c_str() );
	return false;
}

// Every capability the backend does not implement yet stays false, so shared code never asks for it.
// gpuTimer and timestamps belong to the queue's timers: the backend's Init sets them.
void idD3D12Device::FillCaps( renderCaps_t & caps ) const {
	caps = {};
	caps.apiName = "Direct3D 12";
	caps.vendorName = VendorName( features.vendorId );
	caps.rendererName = features.adapterName.c_str();
	caps.versionString = features.versionString.c_str();
	caps.maxTextureSize = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	caps.maxAnisotropy = static_cast<float>( D3D12_MAX_MAXANISOTROPY );
	caps.uniformBufferOffsetAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
	for ( size_t i = 0; i < features.samples.size(); i++ ) {
		if ( features.samples[i] ) {
			caps.maxSamples = 1 << i;
		}
	}
	caps.depthBits = 24;
	caps.maxDebugGroupDepth = idD3D12DebugGroups::MAX_DEPTH;
	caps.depthBoundsTest = features.depthBounds;
	caps.maskedColorClear = true;
	caps.quadBufferStereo = false;
	caps.adaptiveSync = false;
	caps.debugGroups = true;
}

int idD3D12Device::DrainMessages() const {
	int errors = 0;
	std::vector<byte> buffer;
	if ( infoQueue ) {
		const UINT64 count = infoQueue->GetNumStoredMessages();
		for ( UINT64 i = 0; i < count; i++ ) {
			SIZE_T length = 0;
			if ( FAILED( infoQueue->GetMessage( i, nullptr, &length ) ) || length == 0 ) {
				continue;
			}
			buffer.resize( length );
			D3D12_MESSAGE * message = reinterpret_cast<D3D12_MESSAGE *>( buffer.data() );
			// the layer stores the clear value warnings despite the storage filter (Windows 11 26300)
			if ( SUCCEEDED( infoQueue->GetMessage( i, message, &length ) ) && std::ranges::find( MUTED_MESSAGES, message->ID ) == std::end( MUTED_MESSAGES ) ) {
				common->Printf( "d3d12 debug: %s #%d %s\n", SeverityName( message->Severity ), static_cast<int>( message->ID ), message->pDescription );
				errors += ( message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ) ? 1 : 0;
			}
		}
		infoQueue->ClearStoredMessages();
	}
	if ( dxgiInfoQueue ) {
		const UINT64 count = dxgiInfoQueue->GetNumStoredMessages( DXGI_DEBUG_ALL );
		for ( UINT64 i = 0; i < count; i++ ) {
			SIZE_T length = 0;
			if ( FAILED( dxgiInfoQueue->GetMessage( DXGI_DEBUG_ALL, i, nullptr, &length ) ) || length == 0 ) {
				continue;
			}
			buffer.resize( length );
			DXGI_INFO_QUEUE_MESSAGE * message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE *>( buffer.data() );
			if ( SUCCEEDED( dxgiInfoQueue->GetMessage( DXGI_DEBUG_ALL, i, message, &length ) ) ) {
				common->Printf( "d3d12 debug: %s #%d DXGI: %s\n", SeverityName( message->Severity ), message->ID, message->pDescription );
				errors += ( message->Severity <= DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR ) ? 1 : 0;
			}
		}
		dxgiInfoQueue->ClearStoredMessages( DXGI_DEBUG_ALL );
	}
	return errors;
}

void idD3D12Device::ReportLoss() const {
	if ( lossReported ) {
		return;
	}
	lossReported = true;
	common->Warning( "d3d12: the device was lost: %s", D3D12_ErrorText( device->GetDeviceRemovedReason() ).c_str() );
	DrainMessages();
	if ( debugLevel >= 3 ) {
		ReportDred();
	} else {
		common->Printf( "d3d12: r_debugContext 3 records where the GPU stopped (DRED)\n" );
	}
}

// The lists DRED still tracks: every unfinished one, which is where the GPU stopped, and the newest.
void idD3D12Device::ReportDred() const {
	idComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if ( FAILED( device.As( &dred ) ) ) {
		common->Printf( "d3d12 dred: the device has no DRED data\n" );
		return;
	}
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
	if ( const HRESULT hr = dred->GetAutoBreadcrumbsOutput( &breadcrumbs ); FAILED( hr ) ) {
		common->Printf( "d3d12 dred: no breadcrumbs: %s\n", D3D12_ErrorText( hr ).c_str() );
	} else {
		int lists = 0;
		int unfinished = 0;
		const D3D12_AUTO_BREADCRUMB_NODE * newest = nullptr;
		for ( const D3D12_AUTO_BREADCRUMB_NODE * node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext ) {
			lists++;
			newest = node;
			const UINT done = ( node->pLastBreadcrumbValue != nullptr ) ? *node->pLastBreadcrumbValue : 0;
			if ( done < node->BreadcrumbCount ) {
				unfinished++;
				PrintBreadcrumb( *node );
			}
		}
		common->Printf( "d3d12 dred: breadcrumbs of %d command lists, %d unfinished%s\n", lists, unfinished, ( newest != nullptr ) ? "; the newest:" : "" );
		if ( newest != nullptr ) {
			PrintBreadcrumb( *newest );
		}
	}
	if ( idComPtr<ID3D12DeviceRemovedExtendedData2> dred2; SUCCEEDED( dred.As( &dred2 ) ) ) {
		common->Printf( "d3d12 dred: the device state is %s\n", DredDeviceState( dred2->GetDeviceState() ) );
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
	if ( SUCCEEDED( dred->GetPageFaultAllocationOutput( &pageFault ) ) && pageFault.PageFaultVA != 0 ) {
		common->Printf( "d3d12 dred: page fault at GPU address 0x%llx\n", static_cast<unsigned long long>( pageFault.PageFaultVA ) );
		PrintAllocations( "a live", pageFault.pHeadExistingAllocationNode );
		PrintAllocations( "a freed", pageFault.pHeadRecentFreedAllocationNode );
	}
}

void idD3D12Device::RemoveDevice() const {
	idComPtr<ID3D12Device5> device5;
	if ( FAILED( device.As( &device5 ) ) ) {
		common->Printf( "d3d12: the device has no ID3D12Device5::RemoveDevice\n" );
		return;
	}
	device5->RemoveDevice();
}

d3d12VideoMemory_t idD3D12Device::VideoMemory() const {
	idComPtr<IDXGIAdapter3> adapter3;
	DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
	if ( FAILED( adapter.As( &adapter3 ) ) || FAILED( adapter3->QueryVideoMemoryInfo( 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info ) ) ) {
		return {};
	}
	return { info.CurrentUsage, info.Budget };
}

void idD3D12Device::PrintInfo() const {
	common->Printf( "\nD3D12 adapter: %s (%s, vendor 0x%04x)%s\n", features.adapterName.c_str(), VendorName( features.vendorId ), features.vendorId, features.warp ? ", software (WARP)" : "" );
	const d3d12VideoMemory_t memory = VideoMemory();
	common->Printf( "D3D12 video memory: %llu MB, %llu MB in use of a %llu MB budget\n", static_cast<unsigned long long>( features.dedicatedVideoMemory >> 20 ),
		static_cast<unsigned long long>( memory.used >> 20 ), static_cast<unsigned long long>( memory.budget >> 20 ) );
	common->Printf( "D3D12 driver: %s, runtime: %s, shader model %s\n", VersionText( features.driverVersion ).c_str(), features.runtime.c_str(),
		ShaderModelText( features.shaderModel ).c_str() );
	idStr samples;
	for ( size_t i = 0; i < features.samples.size(); i++ ) {
		if ( features.samples[i] ) {
			samples += va( " %d", 1 << i );
		}
	}
	common->Printf( "D3D12 MSAA sample counts:%s\n", samples.c_str() );
	common->Printf( "D3D12 features: depth bounds %d, unaligned BC blocks %d, GPU upload heap %d, dynamic depth bias %d, aniso with point mip %d, tearing %d\n",
		features.depthBounds, features.unalignedBlocks, features.gpuUploadHeap, features.dynamicDepthBias, features.anisoPointMip, features.tearing );
	common->Printf( "D3D12 debug level: %d%s\n", debugLevel, ( debugLevel > 0 && !dxgiInfoQueue ) ? " (no DXGI debug layer)" : "" );
}

// The messages of the teardown first. The device itself is always alive here and reported with its
// references: that line is not a leak.
void idD3D12Device::ReportLiveObjects() const {
	DrainMessages();
#ifdef _DEBUG
	if ( !infoQueue ) {
		return;
	}
	idComPtr<ID3D12DebugDevice> debugDevice;
	if ( FAILED( device.As( &debugDevice ) ) ) {
		return;
	}
	debugDevice->ReportLiveDeviceObjects( D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL );
	debugDevice.Reset();

	int live = 0;
	std::vector<byte> buffer;
	const UINT64 count = infoQueue->GetNumStoredMessages();
	for ( UINT64 i = 0; i < count; i++ ) {
		SIZE_T length = 0;
		if ( FAILED( infoQueue->GetMessage( i, nullptr, &length ) ) || length == 0 ) {
			continue;
		}
		buffer.resize( length );
		D3D12_MESSAGE * message = reinterpret_cast<D3D12_MESSAGE *>( buffer.data() );
		if ( SUCCEEDED( infoQueue->GetMessage( i, message, &length ) ) && message->ID != D3D12_MESSAGE_ID_LIVE_DEVICE ) {
			common->Printf( "d3d12 live: #%d %s\n", static_cast<int>( message->ID ), message->pDescription );
			live++;
		}
	}
	infoQueue->ClearStoredMessages();
	common->Printf( "d3d12: %d live objects at shutdown\n", live );
#endif
}

idD3D12Queue::idD3D12Queue( const idD3D12Device & owner ) :
	device( owner ) {
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	device.Check( device.Get().CreateCommandQueue( &desc, IID_PPV_ARGS( &queue ) ), "CreateCommandQueue" );
	device.Check( device.Get().CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( &fence ) ), "CreateFence" );
	queue->SetName( L"doom3 direct queue" );
	event.reset( CreateEventW( nullptr, FALSE, FALSE, nullptr ) );
	if ( !event ) {
		common->FatalError( "d3d12: CreateEvent failed" );
	}
}

// The value advances even when Signal fails on a lost device, so that Wait returns.
uint64 idD3D12Queue::Execute( ID3D12CommandList & list ) {
	ID3D12CommandList * lists[] = { &list };
	queue->ExecuteCommandLists( 1, lists );
	return Signal();
}

uint64 idD3D12Queue::Signal() {
	lastSignaled++;
	device.Check( queue->Signal( fence.Get(), lastSignaled ), "Signal" );
	return lastSignaled;
}

void idD3D12Queue::Wait( uint64 value ) {
	value = Min( value, lastSignaled );		// nothing will ever signal a later value
	if ( value <= waited ) {
		return;
	}
	if ( !device.IsLost() && fence->GetCompletedValue() < value ) {
		if ( device.Check( fence->SetEventOnCompletion( value, event.get() ), "SetEventOnCompletion" ) ) {
			// a hung GPU shows as a removed device, which the second check reports
			while ( WaitForSingleObject( event.get(), 1000 ) == WAIT_TIMEOUT && device.Check( device.Get().GetDeviceRemovedReason(), "the GPU" ) ) {
			}
		}
	}
	waited = value;
}

uint64 idD3D12Queue::TimestampFrequency() const {
	UINT64 frequency = 0;
	device.Check( queue->GetTimestampFrequency( &frequency ), "GetTimestampFrequency" );
	return frequency;
}

void idD3D12Graveyard::Bury( idComPtr<ID3D12Pageable> object ) {
	if ( object ) {
		graves.push_back( { queue.LastSignaled() + 1, std::move( object ) } );
	}
}

// A view recorded into a list (OMSetRenderTargets) or copied into a table is read then, but the slot
// still waits: R3 releases every descriptor slot after the next Present.
void idD3D12Graveyard::Bury( idD3D12DescriptorPool & pool, d3d12Descriptor_t slot ) {
	if ( slot.IsValid() ) {
		graves.push_back( { queue.LastSignaled() + 1, nullptr, &pool, slot } );
	}
}

void idD3D12Graveyard::Release( grave_t & grave ) {
	if ( grave.pool != nullptr ) {
		grave.pool->Free( grave.slot );
		grave.pool = nullptr;
	}
}

void idD3D12Graveyard::Collect() {
	while ( !graves.empty() && graves.front().fence <= queue.Waited() ) {
		Release( graves.front() );
		graves.pop_front();
	}
}

void idD3D12Graveyard::CollectAll() {
	for ( grave_t & grave : graves ) {
		Release( grave );
	}
	graves.clear();
}

idD3D12CommandStream::idD3D12CommandStream( const idD3D12Device & owner, idD3D12Queue & commandQueue ) :
	device( owner ),
	queue( commandQueue ) {
}

ID3D12GraphicsCommandList1 & idD3D12CommandStream::Open() {
	assert( !open );
	if ( !retired.empty() && retired.front().fence <= queue.Waited() ) {
		current = std::move( retired.front().allocator );
		retired.pop_front();
	} else {
		device.Check( device.Get().CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS( &current ) ), "CreateCommandAllocator" );
	}
	device.Check( current->Reset(), "ID3D12CommandAllocator::Reset" );
	if ( !list ) {
		device.Check( device.Get().CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT, current.Get(), nullptr, IID_PPV_ARGS( &list ) ), "CreateCommandList" );
		list->SetName( L"doom3 frame list" );
	} else {
		device.Check( list->Reset( current.Get(), nullptr ), "ID3D12GraphicsCommandList::Reset" );
	}
	serial++;
	open = true;
	return *list.Get();
}

ID3D12GraphicsCommandList1 & idD3D12CommandStream::List() {
	assert( open );
	return *list.Get();
}

// A list that failed to close (the device was removed) is not executed; the fence still advances.
uint64 idD3D12CommandStream::CloseAndExecute() {
	assert( open );
	open = false;
	const bool closed = device.Check( list->Close(), "ID3D12GraphicsCommandList::Close" );
	const uint64 fence = closed ? queue.Execute( *list.Get() ) : queue.Signal();
	retired.push_back( { std::move( current ), fence } );
	return fence;
}

void idD3D12CommandStream::Abandon() {
	if ( !open ) {
		return;
	}
	open = false;
	list->Close();
	current.Reset();
}
