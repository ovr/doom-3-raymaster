// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include <bit>

#include "../tr_local.h"
#include "d3d12_Dxc.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_Nrd.h"
#include "d3d12_Output.h"

#if defined( USE_NRD )
#include <NRD.h>
#endif

#if !defined( USE_NRD )

struct idD3D12Nrd::denoiser_t {};

idD3D12Nrd::idD3D12Nrd( const idD3D12Device & owner, const idD3D12Library & dll, idD3D12Graveyard & graves, idD3D12DescriptorPool & srvs,
	idD3D12InternalPasses & internalPasses, const idD3D12ComputeRootSignature & root ) :
	device( owner ), library( dll ), graveyard( graves ), srvPool( srvs ), passes( internalPasses ), computeRoot( root ) {
}

idD3D12Nrd::~idD3D12Nrd() = default;

bool idD3D12Nrd::Available() {
	return false;
}

std::optional<d3d12GraphResource_t> idD3D12Nrd::Reflections( idD3D12RenderGraph &, idD3D12SceneTarget &, const rtReflectionSettings_t &,
	const rtView_t &, const renderRect_t &, d3d12GraphResource_t ) {
	return std::nullopt;
}

void idD3D12Nrd::DrawValidation( idD3D12RenderGraph &, idD3D12SceneTarget &, const renderRect_t &, idD3D12DebugGroups * ) {
}

void idD3D12Nrd::Drop() {
}

#else

namespace {

constexpr float UNITS_PER_METRE = 39.37f;
constexpr float DENOISING_RANGE = 1.0e6f;
constexpr float SKY_VIEW_Z = 1.0e7f;		// past the denoising range: NRD leaves the sky out
constexpr float DEFAULT_FRAME_MS = 1000.0f / 60.0f;
constexpr nrd::Identifier IDENTIFIER = 0;
constexpr size_t GUIDE_VIEW_Z = 0;
constexpr size_t GUIDE_NORMAL_ROUGHNESS = 1;
constexpr size_t GUIDE_MOTION = 2;
constexpr std::array<DXGI_FORMAT, 3> GUIDE_FORMATS = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
constexpr std::array<const char *, 3> GUIDE_NAMES = { "nrd view z", "nrd normal roughness", "nrd motion" };
constexpr DXGI_FORMAT SIGNAL_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT VALIDATION_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

// b0 of internal/nrdGuides.hlsl.
struct nrdGuidesParms_t {
	float							clipToWorld[4][4];
	float							clipW[4];
	float							prevClipW[4];
	float							origin[4];
	float							viewport[4];
	float							limits[4];		// the roughness that reflects nothing, the fade, the mirror's, the sky's view z
	int32							first[2];
	uint32							size[2];
	uint32							depthSrv;
	uint32							normalSrv;
	uint32							materialSrv;
	uint32							motionSrv;
	uint32							albedoSrv;
	uint32							viewZUav;
	uint32							normalRoughnessUav;
	uint32							motionUav;
};
static_assert( sizeof( nrdGuidesParms_t ) == 192 );

// b0 of internal/nrdReflectPack.hlsl.
struct nrdReflectPackParms_t {
	float							hitDistParams[4];
	uint32							size[2];
	uint32							raysSrv;
	uint32							viewZSrv;
	uint32							normalRoughnessSrv;
	uint32							outputUav;
	uint32							unused[2];
};
static_assert( sizeof( nrdReflectPackParms_t ) == 48 );

// b0 of internal/nrdReflectUnpack.hlsl.
struct nrdReflectUnpackParms_t {
	uint32							size[2];
	uint32							denoisedSrv;
	uint32							raysSrv;
	uint32							normalRoughnessSrv;
	uint32							outputUav;
	uint32							flags;			// bit 0: the frame's rays are there
	uint32							unused;
};
static_assert( sizeof( nrdReflectUnpackParms_t ) == 32 );

// b1 of internal/nrdValidation.hlsl.
struct nrdValidationParms_t {
	int32							first[2];
	uint32							size[2];
	uint32							validationSrv;
	uint32							unused[3];
};
static_assert( sizeof( nrdValidationParms_t ) == 32 );

struct nrdFormat_t {
	DXGI_FORMAT						format;
	uint32							bytes;
};

[[nodiscard]] nrdFormat_t NrdFormat( nrd::Format format ) {
	switch ( format ) {
		case nrd::Format::R8_UNORM:				return { DXGI_FORMAT_R8_UNORM, 1 };
		case nrd::Format::R8_SNORM:				return { DXGI_FORMAT_R8_SNORM, 1 };
		case nrd::Format::R8_UINT:				return { DXGI_FORMAT_R8_UINT, 1 };
		case nrd::Format::R8_SINT:				return { DXGI_FORMAT_R8_SINT, 1 };
		case nrd::Format::RG8_UNORM:			return { DXGI_FORMAT_R8G8_UNORM, 2 };
		case nrd::Format::RG8_SNORM:			return { DXGI_FORMAT_R8G8_SNORM, 2 };
		case nrd::Format::RG8_UINT:				return { DXGI_FORMAT_R8G8_UINT, 2 };
		case nrd::Format::RG8_SINT:				return { DXGI_FORMAT_R8G8_SINT, 2 };
		case nrd::Format::RGBA8_UNORM:			return { DXGI_FORMAT_R8G8B8A8_UNORM, 4 };
		case nrd::Format::RGBA8_SNORM:			return { DXGI_FORMAT_R8G8B8A8_SNORM, 4 };
		case nrd::Format::RGBA8_UINT:			return { DXGI_FORMAT_R8G8B8A8_UINT, 4 };
		case nrd::Format::RGBA8_SINT:			return { DXGI_FORMAT_R8G8B8A8_SINT, 4 };
		case nrd::Format::RGBA8_SRGB:			return { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4 };
		case nrd::Format::R16_UNORM:			return { DXGI_FORMAT_R16_UNORM, 2 };
		case nrd::Format::R16_SNORM:			return { DXGI_FORMAT_R16_SNORM, 2 };
		case nrd::Format::R16_UINT:				return { DXGI_FORMAT_R16_UINT, 2 };
		case nrd::Format::R16_SINT:				return { DXGI_FORMAT_R16_SINT, 2 };
		case nrd::Format::R16_SFLOAT:			return { DXGI_FORMAT_R16_FLOAT, 2 };
		case nrd::Format::RG16_UNORM:			return { DXGI_FORMAT_R16G16_UNORM, 4 };
		case nrd::Format::RG16_SNORM:			return { DXGI_FORMAT_R16G16_SNORM, 4 };
		case nrd::Format::RG16_UINT:			return { DXGI_FORMAT_R16G16_UINT, 4 };
		case nrd::Format::RG16_SINT:			return { DXGI_FORMAT_R16G16_SINT, 4 };
		case nrd::Format::RG16_SFLOAT:			return { DXGI_FORMAT_R16G16_FLOAT, 4 };
		case nrd::Format::RGBA16_UNORM:			return { DXGI_FORMAT_R16G16B16A16_UNORM, 8 };
		case nrd::Format::RGBA16_SNORM:			return { DXGI_FORMAT_R16G16B16A16_SNORM, 8 };
		case nrd::Format::RGBA16_UINT:			return { DXGI_FORMAT_R16G16B16A16_UINT, 8 };
		case nrd::Format::RGBA16_SINT:			return { DXGI_FORMAT_R16G16B16A16_SINT, 8 };
		case nrd::Format::RGBA16_SFLOAT:		return { DXGI_FORMAT_R16G16B16A16_FLOAT, 8 };
		case nrd::Format::R32_UINT:				return { DXGI_FORMAT_R32_UINT, 4 };
		case nrd::Format::R32_SINT:				return { DXGI_FORMAT_R32_SINT, 4 };
		case nrd::Format::R32_SFLOAT:			return { DXGI_FORMAT_R32_FLOAT, 4 };
		case nrd::Format::RG32_UINT:			return { DXGI_FORMAT_R32G32_UINT, 8 };
		case nrd::Format::RG32_SINT:			return { DXGI_FORMAT_R32G32_SINT, 8 };
		case nrd::Format::RG32_SFLOAT:			return { DXGI_FORMAT_R32G32_FLOAT, 8 };
		case nrd::Format::RGB32_UINT:			return { DXGI_FORMAT_R32G32B32_UINT, 12 };
		case nrd::Format::RGB32_SINT:			return { DXGI_FORMAT_R32G32B32_SINT, 12 };
		case nrd::Format::RGB32_SFLOAT:			return { DXGI_FORMAT_R32G32B32_FLOAT, 12 };
		case nrd::Format::RGBA32_UINT:			return { DXGI_FORMAT_R32G32B32A32_UINT, 16 };
		case nrd::Format::RGBA32_SINT:			return { DXGI_FORMAT_R32G32B32A32_SINT, 16 };
		case nrd::Format::RGBA32_SFLOAT:		return { DXGI_FORMAT_R32G32B32A32_FLOAT, 16 };
		case nrd::Format::R10_G10_B10_A2_UNORM:	return { DXGI_FORMAT_R10G10B10A2_UNORM, 4 };
		case nrd::Format::R10_G10_B10_A2_UINT:	return { DXGI_FORMAT_R10G10B10A2_UINT, 4 };
		case nrd::Format::R11_G11_B10_UFLOAT:	return { DXGI_FORMAT_R11G11B10_FLOAT, 4 };
		case nrd::Format::R9_G9_B9_E5_UFLOAT:	return { DXGI_FORMAT_R9G9B9E5_SHAREDEXP, 4 };
		default:								return { DXGI_FORMAT_UNKNOWN, 0 };
	}
}

// GL's projection (columns one after the other) with its clip z made D3D's: z' = ( z + w ) / 2.
void D3DProjection( const float ( &gl )[16], float ( &d3d )[16] ) {
	for ( int column = 0; column < 4; column++ ) {
		for ( int row = 0; row < 4; row++ ) {
			d3d[column * 4 + row] = gl[column * 4 + row];
		}
		d3d[column * 4 + 2] = 0.5f * ( gl[column * 4 + 2] + gl[column * 4 + 3] );
	}
}

// REBLUR's normalization of a hit distance, in world units; internal/nrdReflectPack.hlsl takes the same numbers.
[[nodiscard]] nrd::ReblurHitDistanceParameters HitDistanceParameters() {
	nrd::ReblurHitDistanceParameters parameters;
	parameters.A *= UNITS_PER_METRE;
	return parameters;
}

[[nodiscard]] constexpr uint32 DivideUp( uint32 size, uint32 factor ) {
	return ( factor > 1 ) ? ( size + factor - 1 ) / factor : size;
}

}	// namespace

struct idD3D12Nrd::denoiser_t {
	nrd::Instance *					instance = nullptr;
	uint32							width = 0;
	uint32							height = 0;
	bool							validation = false;		// OUT_VALIDATION is there and NRD writes it
	std::vector<texture_t>			permanent;
	std::vector<texture_t>			transient;
	std::array<texture_t, static_cast<size_t>( nrd::ResourceType::MAX_NUM )>	io;	// the signal's own, by type
	std::unordered_map<std::string, table_t>	tables;	// by pipeline and textures
	std::unordered_set<std::string>	names;					// the graph keeps the passes' names
	std::vector<byte>				constants;				// the last dispatch's
	float							smoothedMs = DEFAULT_FRAME_MS;
	uint32							epoch = 0;
	uint32							sequence = 0;
	bool							ran = false;			// the output holds epoch and sequence
	bool							fresh = true;			// new textures: CLEAR_AND_RESTART
};

idD3D12Nrd::idD3D12Nrd( const idD3D12Device & owner, const idD3D12Library & dll, idD3D12Graveyard & graves, idD3D12DescriptorPool & srvs,
	idD3D12InternalPasses & internalPasses, const idD3D12ComputeRootSignature & root ) :
	device( owner ),
	library( dll ),
	graveyard( graves ),
	srvPool( srvs ),
	passes( internalPasses ),
	computeRoot( root ) {
}

// The context has waited for the queue: what is left dies with it.
idD3D12Nrd::~idD3D12Nrd() {
	for ( auto & eye : denoisers ) {
		for ( std::unique_ptr<denoiser_t> & denoiser : eye ) {
			if ( denoiser && denoiser->instance != nullptr ) {
				nrd::DestroyInstance( *denoiser->instance );
			}
		}
	}
}

bool idD3D12Nrd::Available() {
	return true;
}

bool idD3D12Nrd::CreateTexture( texture_t & texture, DXGI_FORMAT format, uint32 width, uint32 height, const wchar_t * name ) {
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = Max( width, 1u );
	desc.Height = Max( height, 1u );
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	texture.state = {};
	texture.state.current = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	if ( !device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, texture.state.current, nullptr,
			IID_PPV_ARGS( &texture.resource ) ), "CreateCommittedResource (nrd)" ) ) {
		return false;
	}
	texture.resource->SetName( name );
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = format;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.Format = format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	texture.srv = srvPool.Allocate();
	srvPool.CreateSrv( texture.srv, *texture.resource.Get(), srv );
	texture.uav = srvPool.Allocate();
	srvPool.CreateUav( texture.uav, *texture.resource.Get(), uav );
	const D3D12_RESOURCE_ALLOCATION_INFO info = device.Get().GetResourceAllocationInfo( 0, 1, &desc );
	texture.bytes = info.SizeInBytes;
	bytes += texture.bytes;
	return true;
}

// A submitted list may still read the views: they go through the graveyard with the resource.
void idD3D12Nrd::BuryTexture( texture_t & texture ) {
	if ( texture.resource ) {
		bytes -= texture.bytes;
		graveyard.Bury( std::move( texture.resource ) );
	}
	for ( d3d12Descriptor_t * view : { &texture.srv, &texture.uav } ) {
		if ( view->IsValid() ) {
			graveyard.Bury( srvPool, *view );
			*view = {};
		}
	}
	texture = {};
}

void idD3D12Nrd::BuryDenoiser( denoiser_t & denoiser ) {
	for ( auto & [key, table] : denoiser.tables ) {
		graveyard.BuryRange( srvPool, table.first, table.count );
	}
	denoiser.tables.clear();
	for ( std::vector<texture_t> * pool : { &denoiser.permanent, &denoiser.transient } ) {
		for ( texture_t & texture : *pool ) {
			BuryTexture( texture );
		}
		pool->clear();
	}
	for ( texture_t & texture : denoiser.io ) {
		BuryTexture( texture );
	}
	if ( denoiser.instance != nullptr ) {
		nrd::DestroyInstance( *denoiser.instance );
		denoiser.instance = nullptr;
	}
}

void idD3D12Nrd::BuryGuides( guides_t & eyeGuides ) {
	for ( texture_t & texture : eyeGuides.textures ) {
		BuryTexture( texture );
	}
	eyeGuides = {};
}

void idD3D12Nrd::Drop() {
	for ( auto & eye : denoisers ) {
		for ( std::unique_ptr<denoiser_t> & denoiser : eye ) {
			if ( denoiser ) {
				BuryDenoiser( *denoiser );
				denoiser.reset();
			}
		}
	}
	for ( guides_t & eyeGuides : guides ) {
		BuryGuides( eyeGuides );
	}
	validationEye = -1;
}

ID3D12RootSignature * idD3D12Nrd::RootSignature( uint32 textures, uint32 storage ) {
	for ( const rootSignature_t & made : rootSignatures ) {
		if ( made.textures == textures && made.storage == storage ) {
			return made.signature.Get();
		}
	}
	std::array<D3D12_DESCRIPTOR_RANGE1, 2> ranges = {};
	UINT numRanges = 0;
	if ( textures > 0 ) {
		ranges[numRanges++] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, textures, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
	}
	if ( storage > 0 ) {
		ranges[numRanges++] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, storage, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
	}
	std::array<D3D12_ROOT_PARAMETER1, 2> parameters = {};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[0].Descriptor.ShaderRegister = 0;
	parameters[0].Descriptor.RegisterSpace = 1;
	parameters[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable = { numRanges, ranges.data() };
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	// NRD's InstanceDesc::samplers, in its order: s0 nearest, s1 linear, both clamped
	std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplerDescs = {};
	for ( UINT i = 0; i < samplerDescs.size(); i++ ) {
		D3D12_STATIC_SAMPLER_DESC & sampler = samplerDescs[i];
		sampler.Filter = ( i == 0 ) ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.ShaderRegister = i;
		sampler.RegisterSpace = 1;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = static_cast<UINT>( parameters.size() );
	desc.Desc_1_1.pParameters = parameters.data();
	desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>( samplerDescs.size() );
	desc.Desc_1_1.pStaticSamplers = samplerDescs.data();
	idComPtr<ID3DBlob> blob;
	idComPtr<ID3DBlob> error;
	const HRESULT hr = library.serializeRootSignature( &desc, &blob, &error );
	if ( FAILED( hr ) ) {
		common->FatalError( "d3d12: the NRD root signature does not serialize: %s %s", D3D12_ErrorText( hr ).c_str(),
			error ? static_cast<const char *>( error->GetBufferPointer() ) : "" );
	}
	rootSignature_t made;
	made.textures = textures;
	made.storage = storage;
	if ( !device.Check( device.Get().CreateRootSignature( 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS( &made.signature ) ),
			"CreateRootSignature (nrd)" ) ) {
		return nullptr;
	}
	made.signature->SetName( L"doom3 nrd root signature" );
	rootSignatures.push_back( std::move( made ) );
	return rootSignatures.back().signature.Get();
}

ID3D12PipelineState * idD3D12Nrd::Pipeline( const void * dxil, uint64 size, ID3D12RootSignature & root ) {
	idComPtr<ID3D12PipelineState> & pso = pipelines[dxil];
	if ( !pso ) {
		if ( !D3D12_DxilSigned( std::span( static_cast<const byte *>( dxil ), static_cast<size_t>( size ) ) ) ) {
			common->FatalError( "d3d12: a shader of NRD is no signed DXIL" );
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = &root;
		desc.CS = { dxil, static_cast<SIZE_T>( size ) };
		if ( device.Check( device.Get().CreateComputePipelineState( &desc, IID_PPV_ARGS( &pso ) ), "CreateComputePipelineState (nrd)" ) ) {
			pso->SetName( L"doom3 nrd" );
		}
	}
	return pso.Get();
}

const d3d12ComputePipeline_t * idD3D12Nrd::ComputePipeline( d3d12ComputePipeline_t & pipeline, const char * stageName, const wchar_t * name ) {
	if ( !pipeline ) {
		const d3d12HlslStage_t & stage = passes.Stage( stageName );
		pipeline = D3D12_CreateComputePipeline( device, computeRoot, stage.code, stage.reflection, name );
	}
	return pipeline ? &pipeline : nullptr;
}

d3d12GraphResource_t idD3D12Nrd::Import( idD3D12RenderGraph & graph, const char * name, texture_t & texture, uint32 width, uint32 height,
	std::unordered_map<const texture_t *, d3d12GraphResource_t> & imported ) {
	const auto [it, added] = imported.try_emplace( &texture );
	if ( added ) {
		it->second = graph.Import( name, { texture.resource.Get(), &texture.state, texture.srv, texture.uav, {}, width, height } );
	}
	return it->second;
}

idD3D12Nrd::guides_t * idD3D12Nrd::Guides( int eye, uint32 width, uint32 height ) {
	guides_t & eyeGuides = guides[eye];
	if ( eyeGuides.textures[0].resource && eyeGuides.width == width && eyeGuides.height == height ) {
		return &eyeGuides;
	}
	BuryGuides( eyeGuides );
	if ( width == 0 || height == 0 || device.IsLost() ) {
		return nullptr;
	}
	static constexpr std::array<const wchar_t *, 3> NAMES = { L"doom3 nrd view z", L"doom3 nrd normal roughness", L"doom3 nrd motion" };
	for ( size_t i = 0; i < eyeGuides.textures.size(); i++ ) {
		if ( !CreateTexture( eyeGuides.textures[i], GUIDE_FORMATS[i], width, height, NAMES[i] ) ) {
			BuryGuides( eyeGuides );
			return nullptr;
		}
	}
	eyeGuides.width = width;
	eyeGuides.height = height;
	return &eyeGuides;
}

idD3D12Nrd::denoiser_t * idD3D12Nrd::Denoiser( signal_t signal, int eye, uint32 width, uint32 height, bool validate ) {
	std::unique_ptr<denoiser_t> & slot = denoisers[eye][static_cast<size_t>( signal )];
	if ( slot && slot->width == width && slot->height == height && slot->validation == validate ) {
		return slot.get();
	}
	if ( slot ) {
		BuryDenoiser( *slot );
		slot.reset();
	}
	if ( width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX || device.IsLost() ) {
		return nullptr;
	}
	auto made = std::make_unique<denoiser_t>();
	made->width = width;
	made->height = height;
	made->validation = validate;
	const nrd::DenoiserDesc denoiserDesc = { IDENTIFIER, nrd::Denoiser::REBLUR_SPECULAR };
	nrd::InstanceCreationDesc creation = {};
	creation.denoisers = &denoiserDesc;
	creation.denoisersNum = 1;
	if ( nrd::CreateInstance( creation, made->instance ) != nrd::Result::SUCCESS || made->instance == nullptr ) {
		common->Warning( "d3d12: NRD made no instance of %s", nrd::GetDenoiserString( denoiserDesc.denoiser ) );
		return nullptr;
	}
	const nrd::InstanceDesc & desc = *nrd::GetInstanceDesc( *made->instance );
	// the root signatures above are made for this layout
	const bool layout = desc.resourcesSpaceIndex == 0 && desc.constantBufferAndSamplersSpaceIndex == 1 && desc.constantBufferRegisterIndex == 0
		&& desc.samplersBaseRegisterIndex == 0 && desc.resourcesBaseRegisterIndex == 0 && desc.samplersNum == 2
		&& desc.samplers[0] == nrd::Sampler::NEAREST_CLAMP && desc.samplers[1] == nrd::Sampler::LINEAR_CLAMP;
	if ( !layout ) {
		common->Warning( "d3d12: NRD's register layout is not the one its root signatures are made for" );
		nrd::DestroyInstance( *made->instance );
		return nullptr;
	}
	const auto createPool = [this, width, height]( std::vector<texture_t> & pool, const nrd::TextureDesc * descs, uint32 count, const wchar_t * name ) {
		pool.resize( count );
		for ( uint32 i = 0; i < count; i++ ) {
			const nrdFormat_t format = NrdFormat( descs[i].format );
			if ( format.format == DXGI_FORMAT_UNKNOWN || !CreateTexture( pool[i], format.format, DivideUp( width, descs[i].downsampleFactor ),
					DivideUp( height, descs[i].downsampleFactor ), name ) ) {
				return false;
			}
		}
		return true;
	};
	bool created = createPool( made->permanent, desc.permanentPool, desc.permanentPoolSize, L"doom3 nrd permanent" )
		&& createPool( made->transient, desc.transientPool, desc.transientPoolSize, L"doom3 nrd transient" );
	const auto createIo = [this, &made, width, height]( nrd::ResourceType type, DXGI_FORMAT format, const wchar_t * name ) {
		return CreateTexture( made->io[static_cast<size_t>( type )], format, width, height, name );
	};
	created = created && createIo( nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, SIGNAL_FORMAT, L"doom3 nrd specular in" )
		&& createIo( nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, SIGNAL_FORMAT, L"doom3 nrd specular out" );
	created = created && ( !validate || createIo( nrd::ResourceType::OUT_VALIDATION, VALIDATION_FORMAT, L"doom3 nrd validation" ) );
	if ( !created ) {
		BuryDenoiser( *made );
		return nullptr;
	}
	slot = std::move( made );
	return slot.get();
}

bool idD3D12Nrd::WriteGuides( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, guides_t & eyeGuides, const rtView_t & view,
	const renderRect_t & rect, const rtReflectionSettings_t & reflection, std::unordered_map<const texture_t *, d3d12GraphResource_t> & imported ) {
	const uint32 width = eyeGuides.width;
	const uint32 height = eyeGuides.height;
	std::array<d3d12GraphResource_t, 3> uses;
	for ( size_t i = 0; i < uses.size(); i++ ) {
		uses[i] = Import( graph, GUIDE_NAMES[i], eyeGuides.textures[i], width, height, imported );
	}
	if ( eyeGuides.written && eyeGuides.epoch == view.epoch && eyeGuides.sequence == view.sequence ) {
		return true;
	}
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	idD3D12SceneTarget::surface_t & normal = scene.GBuffer( 0 );
	idD3D12SceneTarget::surface_t & material = scene.GBuffer( 1 );
	idD3D12SceneTarget::surface_t & motion = scene.GBuffer( 2 );
	idD3D12SceneTarget::surface_t & albedo = scene.GBuffer( 3 );
	if ( !depth.resource || !normal.resource || !material.resource || !motion.resource || !albedo.resource ) {
		return false;
	}
	const bool multisampled = scene.Samples() > 1;
	const d3d12ComputePipeline_t * pipeline = ComputePipeline( guidesPipeline[multisampled ? 1 : 0],
		multisampled ? "internal_nrdGuides_msaa" : "internal_nrdGuides", L"doom3 nrd guides" );
	if ( pipeline == nullptr ) {
		return false;
	}
	const uint32 sceneWidth = static_cast<uint32>( scene.Width() );
	const uint32 sceneHeight = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, sceneWidth, sceneHeight } );
	const auto importScene = [&graph, sceneWidth, sceneHeight]( const char * name, idD3D12SceneTarget::surface_t & surface ) {
		return graph.Import( name, { surface.resource.Get(), &surface.state, surface.srv, {}, {}, sceneWidth, sceneHeight } );
	};
	const d3d12GraphResource_t normalUse = importScene( "GBuffer normal", normal );
	const d3d12GraphResource_t materialUse = importScene( "GBuffer material", material );
	const d3d12GraphResource_t motionUse = importScene( "GBuffer motion", motion );
	const d3d12GraphResource_t albedoUse = importScene( "GBuffer albedo", albedo );

	nrdGuidesParms_t base = {};
	memcpy( base.clipToWorld, view.clipToWorld, sizeof( base.clipToWorld ) );
	memcpy( base.clipW, view.worldToClip[3], sizeof( base.clipW ) );
	memcpy( base.prevClipW, view.prevWorldToClip[3], sizeof( base.prevClipW ) );
	base.origin[0] = view.origin[0];
	base.origin[1] = view.origin[1];
	base.origin[2] = view.origin[2];
	base.viewport[0] = static_cast<float>( rect.x );
	base.viewport[1] = static_cast<float>( rect.y );
	base.viewport[2] = static_cast<float>( rect.width );
	base.viewport[3] = static_cast<float>( rect.height );
	base.limits[0] = reflection.maxRoughness;
	base.limits[1] = rtReflectionSettings_t::FADE;
	base.limits[2] = reflection.glossy ? reflection.mirrorRoughness : 2.0f;
	base.limits[3] = SKY_VIEW_Z;
	base.first[0] = rect.x;
	base.first[1] = rect.y;
	base.size[0] = width;
	base.size[1] = height;
	graph.AddPass( "nrd guides", d3d12GraphPass_t::COMPUTE, { { depthUse, d3d12GraphAccess_t::DEPTH_READ }, { normalUse, d3d12GraphAccess_t::SRV },
		{ materialUse, d3d12GraphAccess_t::SRV }, { motionUse, d3d12GraphAccess_t::SRV }, { albedoUse, d3d12GraphAccess_t::SRV },
		{ uses[GUIDE_VIEW_Z], d3d12GraphAccess_t::UAV, 0 }, { uses[GUIDE_NORMAL_ROUGHNESS], d3d12GraphAccess_t::UAV, 0 },
		{ uses[GUIDE_MOTION], d3d12GraphAccess_t::UAV, 0 } },
		[pipeline, base, depthUse, normalUse, materialUse, motionUse, albedoUse, uses]( idD3D12GraphContext & context ) {
			nrdGuidesParms_t parms = base;
			parms.depthSrv = context.Srv( depthUse );
			parms.normalSrv = context.Srv( normalUse );
			parms.materialSrv = context.Srv( materialUse );
			parms.motionSrv = context.Srv( motionUse );
			parms.albedoSrv = context.Srv( albedoUse );
			parms.viewZUav = context.Uav( uses[GUIDE_VIEW_Z] );
			parms.normalRoughnessUav = context.Uav( uses[GUIDE_NORMAL_ROUGHNESS] );
			parms.motionUav = context.Uav( uses[GUIDE_MOTION] );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Dispatch( *pipeline, &span, parms.size[0], parms.size[1] );
		} );
	eyeGuides.written = true;
	eyeGuides.epoch = view.epoch;
	eyeGuides.sequence = view.sequence;
	return true;
}

bool idD3D12Nrd::AddDispatches( idD3D12RenderGraph & graph, denoiser_t & denoiser, guides_t & eyeGuides, const rtView_t & view, bool restart,
	std::unordered_map<const texture_t *, d3d12GraphResource_t> & imported ) {
	const uint16 width = static_cast<uint16>( denoiser.width );
	const uint16 height = static_cast<uint16>( denoiser.height );
	const float frameMs = ( view.frameMs > 0.0f ) ? view.frameMs : DEFAULT_FRAME_MS;
	if ( restart || denoiser.fresh ) {
		denoiser.smoothedMs = frameMs;
	} else {
		// NRD's low pass of the frame time (NRDSettings.h): a stutter changes the history's length little
		const float n = 0.2f * 1000.0f / denoiser.smoothedMs;
		denoiser.smoothedMs += ( frameMs - denoiser.smoothedMs ) / ( 1.0f + n );
	}

	nrd::CommonSettings settings = {};
	D3DProjection( view.viewToClip, settings.viewToClipMatrix );
	D3DProjection( view.prevViewToClip, settings.viewToClipMatrixPrev );
	memcpy( settings.worldToViewMatrix, view.worldToView, sizeof( settings.worldToViewMatrix ) );
	memcpy( settings.worldToViewMatrixPrev, view.prevWorldToView, sizeof( settings.worldToViewMatrixPrev ) );
	settings.motionVectorScale[0] = 1.0f;
	settings.motionVectorScale[1] = 1.0f;
	settings.motionVectorScale[2] = 1.0f;
	settings.resourceSize[0] = settings.resourceSizePrev[0] = settings.rectSize[0] = settings.rectSizePrev[0] = width;
	settings.resourceSize[1] = settings.resourceSizePrev[1] = settings.rectSize[1] = settings.rectSizePrev[1] = height;
	settings.timeDeltaBetweenFrames = frameMs;
	settings.denoisingRange = DENOISING_RANGE;
	settings.frameIndex = view.sequence;
	settings.accumulationMode = denoiser.fresh ? nrd::AccumulationMode::CLEAR_AND_RESTART
		: ( restart ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE );
	settings.enableValidation = denoiser.validation;
	if ( nrd::SetCommonSettings( *denoiser.instance, settings ) != nrd::Result::SUCCESS ) {
		return false;
	}

	// NRD's accumulation in seconds, as its defaults at 60 frames a second
	const float fps = 1000.0f / Max( denoiser.smoothedMs, 0.1f );
	nrd::ReblurSettings reblur;
	reblur.hitDistanceParameters = HitDistanceParameters();
	reblur.maxAccumulatedFrameNum = idMath::ClampInt( 1, static_cast<int>( nrd::REBLUR_MAX_HISTORY_FRAME_NUM ),
		static_cast<int>( nrd::GetMaxAccumulatedFrameNum( nrd::REBLUR_DEFAULT_ACCUMULATION_TIME, fps ) ) );
	reblur.maxFastAccumulatedFrameNum = Max( reblur.maxAccumulatedFrameNum / 5, 1u );
	reblur.maxStabilizedFrameNum = reblur.maxAccumulatedFrameNum;
	reblur.historyFixFrameNum = Min( reblur.historyFixFrameNum, reblur.maxFastAccumulatedFrameNum - 1 );
	// a glossy pixel (material 1 of the guides) takes no mirror and no pixel that reflects nothing
	reblur.minMaterialForSpecular = 0.0f;
	if ( nrd::SetDenoiserSettings( *denoiser.instance, IDENTIFIER, &reblur ) != nrd::Result::SUCCESS ) {
		return false;
	}

	const nrd::DispatchDesc * dispatches = nullptr;
	uint32 numDispatches = 0;
	if ( nrd::GetComputeDispatches( *denoiser.instance, &IDENTIFIER, 1, dispatches, numDispatches ) != nrd::Result::SUCCESS ) {
		return false;
	}
	const nrd::InstanceDesc & desc = *nrd::GetInstanceDesc( *denoiser.instance );
	for ( uint32 d = 0; d < numDispatches; d++ ) {
		const nrd::DispatchDesc & dispatch = dispatches[d];
		const nrd::PipelineDesc & pipelineDesc = desc.pipelines[dispatch.pipelineIndex];
		uint32 numTextures = 0;
		uint32 numStorage = 0;
		for ( uint32 r = 0; r < pipelineDesc.resourceRangesNum; r++ ) {
			const nrd::ResourceRangeDesc & range = pipelineDesc.resourceRanges[r];
			const bool storage = ( range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE );
			// the tables put the textures first
			if ( !storage && numStorage > 0 ) {
				return false;
			}
			( storage ? numStorage : numTextures ) += range.descriptorsNum;
		}
		if ( dispatch.resourcesNum != numTextures + numStorage || dispatch.resourcesNum == 0 ) {
			return false;
		}
		ID3D12RootSignature * root = RootSignature( numTextures, numStorage );
		ID3D12PipelineState * pso = ( root != nullptr )
			? Pipeline( pipelineDesc.computeShaderDXIL.bytecode, pipelineDesc.computeShaderDXIL.size, *root ) : nullptr;
		if ( pso == nullptr ) {
			return false;
		}

		std::vector<texture_t *> textures( dispatch.resourcesNum );
		std::vector<d3d12GraphUse_t> uses;
		std::string key = std::to_string( dispatch.pipelineIndex );
		for ( uint32 r = 0; r < dispatch.resourcesNum; r++ ) {
			const nrd::ResourceDesc & resource = dispatch.resources[r];
			texture_t * texture = nullptr;
			const char * name = nrd::GetResourceTypeString( resource.type );
			switch ( resource.type ) {
				case nrd::ResourceType::IN_VIEWZ:				texture = &eyeGuides.textures[GUIDE_VIEW_Z]; break;
				case nrd::ResourceType::IN_NORMAL_ROUGHNESS:	texture = &eyeGuides.textures[GUIDE_NORMAL_ROUGHNESS]; break;
				case nrd::ResourceType::IN_MV:					texture = &eyeGuides.textures[GUIDE_MOTION]; break;
				case nrd::ResourceType::PERMANENT_POOL:
					texture = ( resource.indexInPool < denoiser.permanent.size() ) ? &denoiser.permanent[resource.indexInPool] : nullptr;
					break;
				case nrd::ResourceType::TRANSIENT_POOL:
					texture = ( resource.indexInPool < denoiser.transient.size() ) ? &denoiser.transient[resource.indexInPool] : nullptr;
					break;
				default:
					texture = &denoiser.io[static_cast<size_t>( resource.type )];
					break;
			}
			if ( texture == nullptr || !texture->resource ) {
				common->Warning( "d3d12: NRD asks for %s, which it was not given", name );
				return false;
			}
			const bool storage = ( resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE );
			textures[r] = texture;
			key += storage ? 'u' : 's';
			key += std::to_string( reinterpret_cast<uintptr_t>( texture ) );
			D3D12_RESOURCE_DESC textureDesc = texture->resource->GetDesc();
			const d3d12GraphResource_t use = Import( graph, name, *texture, static_cast<uint32>( textureDesc.Width ), textureDesc.Height, imported );
			const d3d12GraphAccess_t access = storage ? d3d12GraphAccess_t::UAV : d3d12GraphAccess_t::SRV;
			const auto same = std::ranges::find_if( uses, [use]( const d3d12GraphUse_t & other ) { return other.resource == use; } );
			if ( same == uses.end() ) {
				uses.push_back( { use, access, storage ? uint16( 0 ) : d3d12GraphUse_t::ALL_MIPS } );
			} else if ( same->access != access ) {
				return false;
			}
		}

		const auto [it, added] = denoiser.tables.try_emplace( key );
		if ( added ) {
			it->second.count = dispatch.resourcesNum;
			it->second.first = srvPool.AllocateRange( dispatch.resourcesNum );
			for ( uint32 r = 0; r < dispatch.resourcesNum; r++ ) {
				const bool storage = ( dispatch.resources[r].descriptorType == nrd::DescriptorType::STORAGE_TEXTURE );
				srvPool.Copy( { it->second.first.index + r }, storage ? textures[r]->uav : textures[r]->srv );
			}
		}
		const D3D12_GPU_DESCRIPTOR_HANDLE table = srvPool.VisibleGpu( it->second.first );

		// NRD writes its memory again at the next call
		if ( !dispatch.constantBufferDataMatchesPreviousDispatch ) {
			denoiser.constants.assign( dispatch.constantBufferData, dispatch.constantBufferData + dispatch.constantBufferDataSize );
		}
		std::vector<byte> constants = pipelineDesc.hasConstantData ? denoiser.constants : std::vector<byte>();
		const char * passName = denoiser.names.emplace( ( dispatch.name != nullptr ) ? dispatch.name : "nrd" ).first->c_str();
		const uint32 groupsX = dispatch.gridWidth;
		const uint32 groupsY = dispatch.gridHeight;
		graph.AddPass( passName, d3d12GraphPass_t::COMPUTE, uses, [root, pso, constants = std::move( constants ), table, groupsX, groupsY](
			idD3D12GraphContext & context ) {
			std::optional<d3d12UploadSpan_t> span;
			if ( !constants.empty() ) {
				span = context.Parms( std::span<const byte>( constants ) );
			}
			context.DispatchTable( *root, *pso, span ? &*span : nullptr, table, groupsX, groupsY );
		} );
	}
	return true;
}

std::optional<d3d12GraphResource_t> idD3D12Nrd::Reflections( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene,
	const rtReflectionSettings_t & settings, const rtView_t & view, const renderRect_t & rect, d3d12GraphResource_t rays ) {
	const int eye = view.store ? ( view.eye & 1 ) : 2;
	// a screenshot's instance lives until the next frame that is none: its graph has been executed then
	if ( view.store && denoisers[2][static_cast<size_t>( signal_t::REFLECTION )] ) {
		BuryDenoiser( *denoisers[2][static_cast<size_t>( signal_t::REFLECTION )] );
		denoisers[2][static_cast<size_t>( signal_t::REFLECTION )].reset();
		BuryGuides( guides[2] );
	}
	const uint32 width = static_cast<uint32>( rect.width );
	const uint32 height = static_cast<uint32>( rect.height );
	guides_t * eyeGuides = Guides( eye, width, height );
	denoiser_t * denoiser = ( eyeGuides != nullptr ) ? Denoiser( signal_t::REFLECTION, eye, width, height, settings.validation ) : nullptr;
	const d3d12ComputePipeline_t * pack = ComputePipeline( reflectPack, "internal_nrdReflectPack", L"doom3 nrd reflect pack" );
	const d3d12ComputePipeline_t * unpack = ComputePipeline( reflectUnpack, "internal_nrdReflectUnpack", L"doom3 nrd reflect unpack" );
	if ( denoiser == nullptr || pack == nullptr || unpack == nullptr ) {
		return std::nullopt;
	}
	std::unordered_map<const texture_t *, d3d12GraphResource_t> imported;
	if ( !WriteGuides( graph, scene, *eyeGuides, view, rect, settings, imported ) ) {
		return std::nullopt;
	}
	const d3d12GraphResource_t viewZ = imported[&eyeGuides->textures[GUIDE_VIEW_Z]];
	const d3d12GraphResource_t normalRoughness = imported[&eyeGuides->textures[GUIDE_NORMAL_ROUGHNESS]];
	const bool trace = rays.IsValid();
	const bool again = denoiser->ran && denoiser->epoch == view.epoch && denoiser->sequence == view.sequence;
	if ( trace && !again ) {
		const d3d12GraphResource_t input = Import( graph, "nrd specular in", denoiser->io[static_cast<size_t>( nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST )],
			width, height, imported );
		nrdReflectPackParms_t packBase = {};
		const nrd::ReblurHitDistanceParameters hitDistance = HitDistanceParameters();
		packBase.hitDistParams[0] = hitDistance.A;
		packBase.hitDistParams[1] = hitDistance.B;
		packBase.hitDistParams[2] = hitDistance.C;
		packBase.size[0] = width;
		packBase.size[1] = height;
		graph.AddPass( "nrd reflect pack", d3d12GraphPass_t::COMPUTE, { { rays, d3d12GraphAccess_t::SRV }, { viewZ, d3d12GraphAccess_t::SRV },
			{ normalRoughness, d3d12GraphAccess_t::SRV }, { input, d3d12GraphAccess_t::UAV, 0 } },
			[pack, packBase, rays, viewZ, normalRoughness, input]( idD3D12GraphContext & context ) {
				nrdReflectPackParms_t parms = packBase;
				parms.raysSrv = context.Srv( rays );
				parms.viewZSrv = context.Srv( viewZ );
				parms.normalRoughnessSrv = context.Srv( normalRoughness );
				parms.outputUav = context.Uav( input );
				const d3d12UploadSpan_t span = context.Parms( parms );
				context.Dispatch( *pack, &span, parms.size[0], parms.size[1] );
			} );
		const bool restart = !settings.history || !denoiser->ran || denoiser->epoch != view.epoch || view.sequence != denoiser->sequence + 1;
		if ( !AddDispatches( graph, *denoiser, *eyeGuides, view, restart, imported ) ) {
			return std::nullopt;
		}
		denoiser->ran = true;
		denoiser->fresh = false;
		validationEye = denoiser->validation ? eye : -1;
		denoiser->epoch = view.epoch;
		denoiser->sequence = view.sequence;
	} else if ( !denoiser->ran || denoiser->epoch != view.epoch ) {
		return std::nullopt;
	}

	const d3d12GraphResource_t denoised = Import( graph, "nrd specular out", denoiser->io[static_cast<size_t>( nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST )],
		width, height, imported );
	d3d12PoolDesc_t desc;
	desc.format = SIGNAL_FORMAT;
	desc.width = width;
	desc.height = height;
	desc.unorderedAccess = true;
	const d3d12GraphResource_t output = graph.Create( "rt reflection nrd", desc );
	std::vector<d3d12GraphUse_t> uses = { { denoised, d3d12GraphAccess_t::SRV }, { normalRoughness, d3d12GraphAccess_t::SRV },
		{ output, d3d12GraphAccess_t::UAV, 0 } };
	if ( trace ) {
		uses.push_back( { rays, d3d12GraphAccess_t::SRV } );
	}
	const nrdReflectUnpackParms_t unpackBase = { { width, height }, 0, 0, 0, 0, trace ? 1u : 0u, 0 };
	graph.AddPass( "nrd reflect unpack", d3d12GraphPass_t::COMPUTE, uses, [unpack, unpackBase, denoised, normalRoughness, output, rays, trace](
		idD3D12GraphContext & context ) {
		nrdReflectUnpackParms_t parms = unpackBase;
		parms.denoisedSrv = context.Srv( denoised );
		parms.raysSrv = trace ? context.Srv( rays ) : 0;
		parms.normalRoughnessSrv = context.Srv( normalRoughness );
		parms.outputUav = context.Uav( output );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Dispatch( *unpack, &span, parms.size[0], parms.size[1] );
	} );
	return output;
}

void idD3D12Nrd::DrawValidation( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, const renderRect_t & viewport,
	idD3D12DebugGroups * groups ) {
	const int eye = std::exchange( validationEye, -1 );
	denoiser_t * denoiser = ( eye >= 0 ) ? denoisers[eye][static_cast<size_t>( signal_t::REFLECTION )].get() : nullptr;
	idD3D12SceneTarget::surface_t & output = scene.Color( false );
	if ( denoiser == nullptr || !denoiser->validation || !denoiser->ran || !output.resource || viewport.x < 0 || viewport.y < 0
			|| denoiser->width != static_cast<uint32>( viewport.width ) || denoiser->height != static_cast<uint32>( viewport.height )
			|| viewport.x + viewport.width > scene.Width() || viewport.y + viewport.height > scene.Height() ) {
		return;
	}
	const UINT samples = scene.Samples();
	idComPtr<ID3D12PipelineState> & pso = validation[std::countr_zero( samples ) & 3];
	if ( !pso ) {
		constexpr uint8 RGB = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		pso = passes.CreatePipeline( passes.Stage( "internal_nrdValidation" ).code, d3d12TargetId_t::Of( false, samples ).ColorView(), samples, RGB,
			L"doom3 nrd validation", d3d12PassBlend_t::ALPHA );
		if ( !pso ) {
			return;
		}
	}
	std::unordered_map<const texture_t *, d3d12GraphResource_t> imported;
	const d3d12GraphResource_t layer = Import( graph, "nrd validation", denoiser->io[static_cast<size_t>( nrd::ResourceType::OUT_VALIDATION )],
		denoiser->width, denoiser->height, imported );
	const d3d12GraphResource_t target = graph.Import( "Output", { output.resource.Get(), &output.state, output.srv, {}, scene.Rtv( false ),
		static_cast<uint32>( scene.Width() ), static_cast<uint32>( scene.Height() ) } );
	const nrdValidationParms_t base = { { viewport.x, viewport.y }, { denoiser->width, denoiser->height }, 0, {} };
	ID3D12PipelineState * state = pso.Get();
	graph.AddPass( "nrd validation", d3d12GraphPass_t::RASTER, { { layer, d3d12GraphAccess_t::SRV }, { target, d3d12GraphAccess_t::RTV, 0 } },
		[state, base, layer, target, viewport]( idD3D12GraphContext & context ) {
			nrdValidationParms_t parms = base;
			parms.validationSrv = context.Srv( layer );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Draw( *state, target, 0, viewport, span );
		} );
	graph.Execute( groups );
}

#endif
