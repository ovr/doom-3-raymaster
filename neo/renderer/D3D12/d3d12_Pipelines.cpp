// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include <string>

#include "../tr_local.h"
#include "d3d12_Pipelines.h"

namespace {

D3D12_ROOT_PARAMETER1 RootCbv( UINT shaderRegister, D3D12_SHADER_VISIBILITY visibility ) {
	D3D12_ROOT_PARAMETER1 parameter = {};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameter.Descriptor.ShaderRegister = shaderRegister;
	// written before the draw is recorded and kept until its fence is waited for
	parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	parameter.ShaderVisibility = visibility;
	return parameter;
}

// A serialization error is the backend's own bug: fatal.
[[nodiscard]] idComPtr<ID3D12RootSignature> CreateRootSignature( const idD3D12Device & device, const idD3D12Library & library,
	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC & desc, const wchar_t * name ) {
	idComPtr<ID3DBlob> blob;
	idComPtr<ID3DBlob> error;
	const HRESULT hr = library.serializeRootSignature( &desc, &blob, &error );
	if ( FAILED( hr ) ) {
		common->FatalError( "d3d12: D3D12SerializeVersionedRootSignature failed: %s %s", D3D12_ErrorText( hr ).c_str(),
			error ? static_cast<const char *>( error->GetBufferPointer() ) : "" );
	}
	idComPtr<ID3D12RootSignature> signature;
	if ( device.Check( device.Get().CreateRootSignature( 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS( &signature ) ),
			"CreateRootSignature" ) ) {
		signature->SetName( name );
	}
	return signature;
}

// A pipeline state stream subobject as the runtime parses it: the type, then the value, pointer aligned.
// The padding the alignment adds is what the stream wants (C4324).
#pragma warning( push )
#pragma warning( disable : 4324 )
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TYPE, typename T>
struct alignas( void * ) d3d12Subobject_t {
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE	type = TYPE;
	T								value = {};
};
#pragma warning( pop )

// DEPTH_STENCIL1 carries DepthBoundsTestEnable, which the plain graphics description lacks.
struct d3d12PipelineStream_t {
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *>		root;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE>					vs;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>					ps;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>					blend;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>							sampleMask;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>			raster;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1>	depthStencil;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC>		inputLayout;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>	topology;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>	targets;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>			depthFormat;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>				sample;
};

[[nodiscard]] std::string_view View( const idStr & text ) {
	return std::string_view( text.c_str(), static_cast<size_t>( text.Length() ) );
}

// "doom3 pso gui.vfp|gui.vfp": the program as PIX and the debug layer name it.
[[nodiscard]] std::wstring PipelineName( const d3d12Program_t & program ) {
	std::wstring name = L"doom3 pso ";
	for ( const char c : View( program.journalName ) ) {
		name.push_back( static_cast<wchar_t>( static_cast<unsigned char>( c ) ) );
	}
	return name;
}

[[nodiscard]] constexpr uint32 FourCC( char a, char b, char c, char d ) {
	return static_cast<uint32>( a ) | ( static_cast<uint32>( b ) << 8 ) | ( static_cast<uint32>( c ) << 16 ) | ( static_cast<uint32>( d ) << 24 );
}

constexpr uint32 JOURNAL_MAGIC = FourCC( 'D', '3', 'P', 'J' );
constexpr uint32 LIBRARY_MAGIC = FourCC( 'D', '3', 'P', 'L' );
constexpr size_t MAX_JOURNAL_NAME = 1024;

// Little-endian fields into a file image.
class idByteWriter {
public:
	template<typename T> void		Put( const T & value ) {
		static_assert( std::is_trivially_copyable_v<T> );
		const byte * bytes = reinterpret_cast<const byte *>( &value );
		data.insert( data.end(), bytes, bytes + sizeof( T ) );
	}
	void							PutBytes( std::string_view text ) { data.insert( data.end(), text.begin(), text.end() ); }
	[[nodiscard]] std::vector<byte>	Take() { return std::move( data ); }
private:
	std::vector<byte>				data;
};

// Fields out of a file image; any read past its end fails, and so does every read after it.
class idByteReader {
public:
	explicit						idByteReader( std::span<const byte> file ) : data( file ) {}
	template<typename T> bool		Get( T & value ) {
		static_assert( std::is_trivially_copyable_v<T> );
		if ( !ok || data.size() - offset < sizeof( T ) ) {
			ok = false;
			return false;
		}
		memcpy( &value, data.data() + offset, sizeof( T ) );
		offset += sizeof( T );
		return true;
	}
	bool							GetText( size_t length, std::string & text ) {
		if ( !ok || data.size() - offset < length ) {
			ok = false;
			return false;
		}
		text.assign( reinterpret_cast<const char *>( data.data() + offset ), length );
		offset += length;
		return true;
	}
	[[nodiscard]] bool				AtEnd() const { return ok && offset == data.size(); }
private:
	std::span<const byte>			data;
	size_t							offset = 0;
	bool							ok = true;
};

// The header of pipelines.bin, then names x 16 bytes of digests, then libraryBytes. The runtime checks the adapter
// and the driver again in CreatePipelineLibrary.
struct d3d12LibraryHeader_t {
	uint32							magic = LIBRARY_MAGIC;
	uint32							version = idD3D12PipelineLibrary::VERSION;
	uint64							driverVersion = 0;
	uint32							vendorId = 0;
	uint32							names = 0;
	uint64							libraryBytes = 0;
	bool							operator==( const d3d12LibraryHeader_t & ) const = default;
};
static_assert( sizeof( d3d12LibraryHeader_t ) == 32 && sizeof( d3d12PipelineName_t ) == 16 );

[[nodiscard]] d3d12LibraryHeader_t LibraryHeader( const d3d12Features_t & features, size_t names, uint64 libraryBytes ) {
	d3d12LibraryHeader_t header;
	header.driverVersion = features.driverVersion;
	header.vendorId = features.vendorId;
	header.names = static_cast<uint32>( names );
	header.libraryBytes = libraryBytes;
	return header;
}

// The journal's form of a normalized key: the bias by value, no program.
[[nodiscard]] d3d12JournalEntry_t JournalEntry( const pipelineKey_t & key, const idD3D12DepthBiasTable & biases ) {
	d3d12JournalEntry_t entry;
	entry.key.stateBits = key.stateBits;
	entry.key.layout = key.layout;
	entry.key.raster = key.raster;
	entry.bias = biases[key.depthBias];
	return entry;
}

[[nodiscard]] UINT TargetSamples( const pipelineKey_t & key ) {
	return D3D12_KeyTarget( key.raster ).Samples();
}

// The container's digest of the whole blob (bytes 4-19, the hash dxil.dll signs); zeros for no stage.
[[nodiscard]] std::array<byte, 16> DxbcChecksum( const d3d12Bytecode_t & code ) {
	std::array<byte, 16> checksum = {};
	if ( code.size() >= 20 ) {
		std::copy_n( code.begin() + 4, checksum.size(), checksum.begin() );
	}
	return checksum;
}

}	// namespace

std::array<wchar_t, 40> d3d12PipelineName_t::Text() const {
	std::array<wchar_t, 40> name = {};
	constexpr std::wstring_view PREFIX = L"doom3 ";
	constexpr wchar_t HEX[] = L"0123456789abcdef";
	size_t length = 0;
	for ( const wchar_t c : PREFIX ) {
		name[length++] = c;
	}
	for ( const byte b : digest ) {
		name[length++] = HEX[b >> 4];
		name[length++] = HEX[b & 15];
	}
	return name;
}

d3d12PipelineName_t D3D12_PipelineLibraryName( const d3d12Program_t & program, const d3d12JournalEntry_t & entry, bool depthBounds ) {
	idByteWriter fields;
	fields.Put( idD3D12PipelineLibrary::VERSION );
	fields.Put( DxbcChecksum( program.vertexCode ) );
	fields.Put( DxbcChecksum( program.fragmentCode ) );
	fields.Put( program.info.attribMask );
	fields.Put( entry.key.stateBits );
	fields.Put( entry.key.layout );
	fields.Put( entry.key.raster );
	fields.Put( entry.bias.bias );
	fields.Put( entry.bias.slope );
	fields.Put( static_cast<uint8>( depthBounds ? 1 : 0 ) );
	const std::vector<byte> bytes = fields.Take();
	MD5_CTX context;
	MD5_Init( &context );
	MD5_Update( &context, bytes.data(), bytes.size() );
	d3d12PipelineName_t name;
	MD5_Final( &context, name.digest.data() );
	return name;
}

// Heap-indexed descriptors and their data are volatile: an upload or a copy may change a texture a recorded draw reads (R4).
idD3D12RootSignature::idD3D12RootSignature( const idD3D12Device & device, const idD3D12Library & library ) {
	D3D12_ROOT_PARAMETER1 resources = {};
	resources.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	resources.Constants = { d3d12DrawResources_t::REGISTER, 0, d3d12DrawResources_t::COUNT };
	resources.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	std::array<D3D12_ROOT_PARAMETER1, D3D12_Root( d3d12RootSlot_t::COUNT )> parameters;
	parameters[D3D12_Root( d3d12RootSlot_t::VERTEX_PARMS )] = RootCbv( 0, D3D12_SHADER_VISIBILITY_VERTEX );
	parameters[D3D12_Root( d3d12RootSlot_t::FRAGMENT_PARMS )] = RootCbv( 1, D3D12_SHADER_VISIBILITY_PIXEL );
	parameters[D3D12_Root( d3d12RootSlot_t::JOINTS )] = RootCbv( 2, D3D12_SHADER_VISIBILITY_VERTEX );
	parameters[D3D12_Root( d3d12RootSlot_t::DRAW_RESOURCES )] = resources;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = static_cast<UINT>( parameters.size() );
	desc.Desc_1_1.pParameters = parameters.data();
	desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

	signature = CreateRootSignature( device, library, desc, L"doom3 root signature" );
}

idD3D12ComputeRootSignature::idD3D12ComputeRootSignature( const idD3D12Device & device, const idD3D12Library & library ) {
	D3D12_ROOT_PARAMETER1 constants = {};
	constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	constants.Constants = { D3D12_ComputeRoot( d3d12ComputeSlot_t::CONSTANTS ), 0, d3d12ComputeConstants_t::COUNT };
	constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	std::array<D3D12_ROOT_PARAMETER1, D3D12_ComputeRoot( d3d12ComputeSlot_t::COUNT )> parameters;
	parameters[D3D12_ComputeRoot( d3d12ComputeSlot_t::PARMS )] = RootCbv( D3D12_ComputeRoot( d3d12ComputeSlot_t::PARMS ), D3D12_SHADER_VISIBILITY_ALL );
	parameters[D3D12_ComputeRoot( d3d12ComputeSlot_t::CONSTANTS )] = constants;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = static_cast<UINT>( parameters.size() );
	desc.Desc_1_1.pParameters = parameters.data();
	desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
	signature = CreateRootSignature( device, library, desc, L"doom3 compute root signature" );
}

d3d12ComputePipeline_t D3D12_CreateComputePipeline( const idD3D12Device & device, const idD3D12ComputeRootSignature & root,
	const d3d12Bytecode_t & code, const d3d12StageReflection_t & reflection, const wchar_t * name ) {
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = root.Get();
	desc.CS = { code.data(), code.size() };
	d3d12ComputePipeline_t pipeline;
	pipeline.threadGroup = reflection.threadGroup;
	if ( device.Check( device.Get().CreateComputePipelineState( &desc, IID_PPV_ARGS( &pipeline.pso ) ), "CreateComputePipelineState" ) ) {
		pipeline.pso->SetName( name );
	}
	return pipeline;
}

uint8 idD3D12DepthBiasTable::Slot( float factor, float units ) {
	if ( factor != lastFactor || units != lastUnits ) {
		lastFactor = factor;
		lastUnits = units;
		lastSlot = Find( D3D12_DepthBiasFor( factor, units ), lastNearest );
	}
	nearest += lastNearest ? 1 : 0;
	return lastSlot;
}

uint8 idD3D12DepthBiasTable::Find( const d3d12DepthBias_t & wanted, bool & nearestOne ) {
	nearestOne = false;
	if ( wanted == d3d12DepthBias_t() ) {
		return 0;
	}
	const std::span used( biases.data(), static_cast<size_t>( count ) );
	if ( const auto found = std::ranges::find( used, wanted ); found != used.end() ) {
		return static_cast<uint8>( found - used.begin() + 1 );
	}
	if ( count < static_cast<int>( biases.size() ) ) {
		biases[count++] = wanted;
		return static_cast<uint8>( count );
	}
	if ( !reportedFull ) {
		common->Warning( "d3d12: all %d depth bias slots are in use, new biases draw with the nearest one (see r_d3d12Stats)", count );
		reportedFull = true;
	}
	nearestOne = true;
	return NearestSlot( wanted );
}

// Factor and units weigh the same: the depth a factor adds depends on the surface's slope, unknown here.
// A tie goes to the lower slot, so the choice repeats from run to run.
uint8 idD3D12DepthBiasTable::NearestSlot( const d3d12DepthBias_t & wanted ) const {
	const auto distance = [&wanted]( const d3d12DepthBias_t & bias ) {
		const double units = static_cast<double>( bias.bias ) - static_cast<double>( wanted.bias );
		const double factor = static_cast<double>( bias.slope ) - static_cast<double>( wanted.slope );
		return units * units + factor * factor;
	};
	uint8 best = 0;
	double bestDistance = distance( d3d12DepthBias_t() );
	for ( int slot = 1; slot <= count; slot++ ) {
		if ( const double candidate = distance( biases[slot - 1] ); candidate < bestDistance ) {
			best = static_cast<uint8>( slot );
			bestDistance = candidate;
		}
	}
	return best;
}

d3d12DepthBias_t idD3D12DepthBiasTable::operator[]( uint8 slot ) const {
	assert( slot <= count );
	return ( slot == 0 || slot > count ) ? d3d12DepthBias_t() : biases[slot - 1];
}

void idD3D12PipelineJournal::Record( std::string_view journalName, const d3d12JournalEntry_t & entry ) {
	auto program = entries.find( journalName );
	if ( program == entries.end() ) {
		program = entries.emplace( std::string( journalName ), std::vector<d3d12JournalEntry_t>() ).first;
	}
	if ( std::ranges::find( program->second, entry ) != program->second.end() ) {
		return;
	}
	program->second.push_back( entry );
	numEntries++;
	dirty = true;
}

std::span<const d3d12JournalEntry_t> idD3D12PipelineJournal::EntriesFor( std::string_view journalName ) const {
	const auto program = entries.find( journalName );
	return ( program != entries.end() ) ? std::span<const d3d12JournalEntry_t>( program->second ) : std::span<const d3d12JournalEntry_t>();
}

// A layout or a raster bit the backend never sets means the file is not one this code wrote.
bool idD3D12PipelineJournal::Read( std::span<const byte> file ) {
	entries.clear();
	numEntries = 0;
	dirty = false;
	idByteReader reader( file );
	uint32 magic = 0;
	uint32 version = 0;
	uint32 programs = 0;
	if ( !reader.Get( magic ) || !reader.Get( version ) || !reader.Get( programs ) || magic != JOURNAL_MAGIC || version != VERSION ) {
		return false;
	}
	constexpr uint8 RASTER_BITS = D3D12_KEY_CULL_BITS | D3D12_KEY_LINES | D3D12_KEY_TARGET_BITS | D3D12_KEY_STRICT_DEPTH | D3D12_KEY_HDR_TARGET;
	bool valid = true;
	for ( uint32 i = 0; i < programs && valid; i++ ) {
		uint32 nameLength = 0;
		std::string name;
		uint32 count = 0;
		valid = reader.Get( nameLength ) && nameLength <= MAX_JOURNAL_NAME && reader.GetText( nameLength, name ) && reader.Get( count );
		std::vector<d3d12JournalEntry_t> & keys = entries[name];
		for ( uint32 k = 0; k < count && valid; k++ ) {
			d3d12JournalEntry_t entry;
			valid = reader.Get( entry.key.stateBits ) && reader.Get( entry.key.layout ) && reader.Get( entry.key.raster ) && reader.Get( entry.bias.bias )
				&& reader.Get( entry.bias.slope ) && entry.key.layout < NUM_VERTEX_LAYOUTS && ( entry.key.raster & ~RASTER_BITS ) == 0;
			keys.push_back( entry );
		}
		numEntries += static_cast<int>( count );
	}
	if ( !valid || !reader.AtEnd() ) {
		entries.clear();
		numEntries = 0;
		return false;
	}
	return true;
}

std::vector<byte> idD3D12PipelineJournal::Write() const {
	idByteWriter writer;
	writer.Put( JOURNAL_MAGIC );
	writer.Put( VERSION );
	writer.Put( static_cast<uint32>( entries.size() ) );
	for ( const auto & [name, keys] : entries ) {
		writer.Put( static_cast<uint32>( name.size() ) );
		writer.PutBytes( name );
		writer.Put( static_cast<uint32>( keys.size() ) );
		for ( const d3d12JournalEntry_t & entry : keys ) {
			writer.Put( entry.key.stateBits );
			writer.Put( entry.key.layout );
			writer.Put( entry.key.raster );
			writer.Put( entry.bias.bias );
			writer.Put( entry.bias.slope );
		}
	}
	return writer.Take();
}

std::optional<idD3D12PipelineLibrary> idD3D12PipelineLibrary::Open( const idD3D12Device & device, std::vector<byte> file ) {
	D3D12_FEATURE_DATA_SHADER_CACHE shaderCache = {};
	if ( !D3D12_CheckFeature( device.Get(), D3D12_FEATURE_SHADER_CACHE, &shaderCache, sizeof( shaderCache ) )
			|| ( shaderCache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY ) == 0 ) {
		return std::nullopt;
	}
	idD3D12PipelineLibrary opened( device );
	const bool hadFile = !file.empty();
	d3d12LibraryHeader_t header;
	if ( file.size() > sizeof( header ) ) {
		memcpy( &header, file.data(), sizeof( header ) );
		const size_t indexBytes = static_cast<size_t>( header.names ) * sizeof( d3d12PipelineName_t );
		const size_t libraryBytes = file.size() - sizeof( header ) - Min( indexBytes, file.size() - sizeof( header ) );
		if ( header == LibraryHeader( device.Features(), header.names, libraryBytes ) && libraryBytes > 0 ) {
			const byte * index = file.data() + sizeof( header );
			for ( uint32 i = 0; i < header.names; i++ ) {
				d3d12PipelineName_t name;
				memcpy( name.digest.data(), index + i * sizeof( name ), sizeof( name ) );
				opened.names.insert( name );
			}
			file.erase( file.begin(), file.begin() + static_cast<ptrdiff_t>( sizeof( header ) + indexBytes ) );
			opened.blob = std::move( file );
		}
	}
	if ( hadFile && opened.blob.empty() ) {
		common->Printf( "d3d12: pipelines.bin is of another driver or format; it is made again\n" );
	}
	HRESULT hr = device.Get().CreatePipelineLibrary( opened.blob.data(), opened.blob.size(), IID_PPV_ARGS( &opened.library ) );
	if ( FAILED( hr ) && !opened.blob.empty() ) {
		// D3D12_ERROR_DRIVER_VERSION_MISMATCH, D3D12_ERROR_ADAPTER_NOT_FOUND or E_INVALIDARG for a corrupt file
		common->Printf( "d3d12: pipelines.bin is not usable (%s); it is made again\n", D3D12_ErrorText( hr ).c_str() );
		opened.blob.clear();
		opened.names.clear();
		hr = device.Get().CreatePipelineLibrary( nullptr, 0, IID_PPV_ARGS( &opened.library ) );
	}
	if ( FAILED( hr ) ) {
		common->Printf( "d3d12: no pipeline library: %s\n", D3D12_ErrorText( hr ).c_str() );
		return std::nullopt;
	}
	opened.library->SetName( L"doom3 pipeline library" );
	opened.fromFile = !opened.blob.empty();
	opened.dirty = hadFile && !opened.fromFile;
	return opened;
}

// E_INVALIDARG: the index names a PSO the library lacks (a damaged file); the caller compiles it then.
idComPtr<ID3D12PipelineState> idD3D12PipelineLibrary::Load( const d3d12PipelineName_t & name, const D3D12_PIPELINE_STATE_STREAM_DESC & desc ) const {
	if ( !names.contains( name ) ) {
		return nullptr;
	}
	idComPtr<ID3D12PipelineState> pso;
	const HRESULT hr = library->LoadPipeline( name.Text().data(), &desc, IID_PPV_ARGS( &pso ) );
	if ( hr == E_INVALIDARG || !device->Check( hr, "LoadPipeline" ) ) {
		return nullptr;
	}
	return pso;
}

// E_INVALIDARG: the name is taken, by a PSO the library could not give for this description.
void idD3D12PipelineLibrary::Store( const d3d12PipelineName_t & name, ID3D12PipelineState & pso ) {
	const HRESULT hr = library->StorePipeline( name.Text().data(), &pso );
	if ( hr == E_INVALIDARG ) {
		if ( !reportedStore ) {
			common->Warning( "d3d12: the pipeline library refused a PSO; delete renderprogs/dxbc/pipelines.bin if this repeats" );
			reportedStore = true;
		}
		return;
	}
	if ( device->Check( hr, "StorePipeline" ) ) {
		names.insert( name );
		dirty = true;
	}
}

std::vector<byte> idD3D12PipelineLibrary::Write() const {
	const size_t bytes = library->GetSerializedSize();
	const d3d12LibraryHeader_t header = LibraryHeader( device->Features(), names.size(), bytes );
	const size_t indexBytes = names.size() * sizeof( d3d12PipelineName_t );
	std::vector<byte> file( sizeof( header ) + indexBytes + bytes );
	memcpy( file.data(), &header, sizeof( header ) );
	byte * index = file.data() + sizeof( header );
	for ( const d3d12PipelineName_t & name : names ) {
		index = std::copy( name.digest.begin(), name.digest.end(), index );
	}
	if ( FAILED( library->Serialize( index, bytes ) ) ) {
		return {};
	}
	return file;
}

idD3D12PipelineFactory::idD3D12PipelineFactory( const idD3D12Device & owner, const idD3D12RootSignature & signature, idD3D12PipelineJournal * keys,
	idD3D12PipelineLibrary * psos ) :
	device( owner ),
	root( signature ),
	journal( keys ),
	library( psos ) {
}

ID3D12PipelineState * idD3D12PipelineFactory::Get( d3d12Program_t & program, const pipelineKey_t & key ) {
	for ( const d3d12Pipeline_t & pipeline : program.pipelines ) {
		if ( pipeline.key == key ) {
			return pipeline.pso.Get();
		}
	}
	const uint64 start = Sys_Microseconds();
	bool fromLibrary = false;
	idComPtr<ID3D12PipelineState> pso = Make( program, key, fromLibrary );
	if ( !pso ) {
		return nullptr;
	}
	created++;
	Count( false, fromLibrary, Sys_Microseconds() - start );
	if ( journal != nullptr ) {
		journal->Record( View( program.journalName ), JournalEntry( key, bias ) );
	}
	program.pipelines.push_back( { key, std::move( pso ) } );
	return program.pipelines.back().pso.Get();
}

// The sample count of a target is fixed at Init, so a key of another one can never be drawn with in this run.
void idD3D12PipelineFactory::Warm( d3d12Program_t & program, UINT samples ) {
	if ( journal == nullptr ) {
		return;
	}
	for ( const d3d12JournalEntry_t & entry : journal->EntriesFor( View( program.journalName ) ) ) {
		if ( TargetSamples( entry.key ) != samples ) {
			continue;
		}
		pipelineKey_t key = entry.key;
		key.program = program.dense;
		key.depthBias = bias.SlotOf( entry.bias );
		key = D3D12_NormalizeKey( key );
		if ( std::ranges::any_of( program.pipelines, [&key]( const d3d12Pipeline_t & pipeline ) { return pipeline.key == key; } ) ) {
			continue;
		}
		const uint64 start = Sys_Microseconds();
		bool fromLibrary = false;
		idComPtr<ID3D12PipelineState> pso = Make( program, key, fromLibrary );
		if ( !pso ) {
			break;
		}
		Count( true, fromLibrary, Sys_Microseconds() - start );
		program.pipelines.push_back( { key, std::move( pso ) } );
	}
}

void idD3D12PipelineFactory::Count( bool warm, bool fromLibrary, uint64 microseconds ) {
	for ( stats_t * stats : { &levelStats, &totalStats } ) {
		stats->drawn += warm ? 0 : 1;
		stats->warmed += warm ? 1 : 0;
		stats->loaded += fromLibrary ? 1 : 0;
		( warm ? stats->warmMicroseconds : stats->drawnMicroseconds ) += microseconds;
	}
	total++;
}

idD3D12PipelineFactory::stats_t idD3D12PipelineFactory::TakeLevelStats() {
	const stats_t taken = levelStats;
	levelStats = {};
	return taken;
}

idComPtr<ID3D12PipelineState> idD3D12PipelineFactory::Make( const d3d12Program_t & program, const pipelineKey_t & key, bool & fromLibrary ) {
	fromLibrary = false;
	const bool depthBounds = device.Features().depthBounds;
	const d3d12PipelineDesc_t desc = D3D12_PipelineDescFor( key, program.info.attribMask, bias[key.depthBias], depthBounds );
	d3d12PipelineStream_t stream;
	stream.root.value = root.Get();
	stream.vs.value = { program.vertexCode.data(), program.vertexCode.size() };
	if ( !program.fragmentCode.empty() ) {
		stream.ps.value = { program.fragmentCode.data(), program.fragmentCode.size() };
	}
	stream.blend.value = desc.blend;
	stream.sampleMask.value = UINT_MAX;
	stream.raster.value = desc.raster;
	stream.depthStencil.value = desc.depthStencil;
	stream.inputLayout.value = { desc.inputs.data(), desc.numInputs };
	stream.topology.value = desc.topology;
	stream.targets.value.NumRenderTargets = 1;
	stream.targets.value.RTFormats[0] = desc.colorFormat;
	stream.depthFormat.value = d3d12TargetId_t::DEPTH;
	stream.sample.value = desc.sample;
	const D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = { sizeof( stream ), &stream };

	d3d12PipelineName_t name;
	idComPtr<ID3D12PipelineState> pso;
	if ( library != nullptr ) {
		name = D3D12_PipelineLibraryName( program, JournalEntry( key, bias ), depthBounds );
		pso = library->Load( name, streamDesc );
		fromLibrary = ( pso != nullptr );
	}
	if ( !pso && device.Check( device.Get().CreatePipelineState( &streamDesc, IID_PPV_ARGS( &pso ) ), "CreatePipelineState" ) && library != nullptr ) {
		library->Store( name, *pso.Get() );
	}
	if ( pso ) {
		pso->SetName( PipelineName( program ).c_str() );
	}
	return pso;
}
