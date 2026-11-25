// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Context.h"

idCVar r_d3d12Adapter( "r_d3d12Adapter", "", CVAR_RENDERER | CVAR_INIT,
	"Direct3D 12 adapter: empty = the high-performance GPU, warp = the software reference, N = adapter N by GPU preference" );
idCVar r_d3d12CaptureBackBuffer( "r_d3d12CaptureBackBuffer", "0", CVAR_RENDERER | CVAR_BOOL,
	"1 = ReadOutputPixels reads the back buffer after the present blit instead of the scene target (checks the blit)" );
idCVar r_d3d12ShaderCache( "r_d3d12ShaderCache", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT,
	"1 = Direct3D 12 keeps the DXIL of every shader in fs_savepath/shaders/cache and compiles only a text it has not seen" );
idCVar r_d3d12ShaderThreads( "r_d3d12ShaderThreads", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT,
	"the most DXC runs at once when Direct3D 12 compiles its shaders: 0 = one per hardware thread, 1 = one after another", 0, 64 );
idCVar r_d3d12PipelineLibrary( "r_d3d12PipelineLibrary", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT,
	"1 = Direct3D 12 keeps its PSOs in fs_savepath/renderprogs/dxbc/pipelines.bin, so the next run loads them instead of compiling" );
idCVar r_d3d12FrameBuffers( "r_d3d12FrameBuffers", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT,
	"where Direct3D 12 keeps the vertex cache's per-frame buffers: 0 = UPLOAD heaps the GPU reads in place, 1 = DEFAULT heaps "
	"with a copy per frame, 2 = GPU_UPLOAD heaps (video memory the CPU writes; needs Resizable BAR)", 0, 2 );

namespace {

constexpr uint64 CONSTANTS_RING_SIZE = 16 << 20;
constexpr uint32 SHADER_VISIBLE_SRVS = 1 << 17;	// every texture view and scene surface at once; ~4 MB of descriptors
constexpr uint64 STAGING_RING_SIZE = 64 << 20;	// four uploader pieces: the GPU copies one while the CPU fills the next
constexpr const char * JOURNAL_FILE = "renderprogs/dxbc/pipelines.journal";
constexpr const char * LIBRARY_FILE = "renderprogs/dxbc/pipelines.bin";

[[nodiscard]] constexpr double Megabytes( uint64 bytes ) {
	return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
}

// A file under fs_savepath, empty when there is none.
[[nodiscard]] std::vector<byte> ReadSaved( const char * path ) {
	void * buffer = nullptr;
	const int length = fileSystem->ReadFile( path, &buffer );
	if ( length <= 0 || buffer == nullptr ) {
		return {};
	}
	const byte * bytes = static_cast<const byte *>( buffer );
	std::vector<byte> data( bytes, bytes + length );
	fileSystem->FreeFile( buffer );
	return data;
}

[[nodiscard]] bool WriteSaved( const char * path, std::span<const byte> data ) {
	return !data.empty() && fileSystem->WriteFile( path, data.data(), static_cast<int>( data.size() ) ) == static_cast<int>( data.size() );
}

[[nodiscard]] idD3D12PipelineJournal LoadJournal() {
	idD3D12PipelineJournal journal;
	if ( const std::vector<byte> file = ReadSaved( JOURNAL_FILE ); !file.empty() && !journal.Read( file ) ) {
		common->Printf( "d3d12: %s is of another version or cut short; it is begun again\n", JOURNAL_FILE );
	}
	return journal;
}

[[nodiscard]] std::optional<idD3D12PipelineLibrary> OpenPipelineLibrary( const idD3D12Device & device ) {
	if ( !r_d3d12PipelineLibrary.GetBool() ) {
		return std::nullopt;
	}
	return idD3D12PipelineLibrary::Open( device, ReadSaved( LIBRARY_FILE ) );
}

[[nodiscard]] d3d12BufferPlacement_t FramePlacement( const idD3D12Device & device ) {
	switch ( r_d3d12FrameBuffers.GetInteger() ) {
		case 1:
			return d3d12BufferPlacement_t::STAGED;
		case 2:
			if ( device.Features().gpuUploadHeap ) {
				return d3d12BufferPlacement_t::GPU_UPLOAD;
			}
			common->Printf( "d3d12: r_d3d12FrameBuffers 2: the adapter has no GPU upload heap (OPTIONS16, Resizable BAR); UPLOAD heaps instead\n" );
			return d3d12BufferPlacement_t::UPLOAD;
		default:
			return d3d12BufferPlacement_t::UPLOAD;
	}
}

}	// namespace

std::expected<std::unique_ptr<idD3D12Context>, idStr> idD3D12Context::Create( const renderBackendInit_t & init ) {
	auto loaded = idD3D12Library::Load();
	if ( !loaded ) {
		return std::unexpected( loaded.error() );
	}
	auto created = idD3D12Device::Create( *loaded, r_d3d12Adapter.GetString(), r_debugContext.GetInteger() );
	if ( !created ) {
		return std::unexpected( created.error() );
	}
	std::unique_ptr<idD3D12Context> context( new idD3D12Context( std::move( *loaded ), std::move( *created ) ) );
	const UINT samples = D3D12_SceneSamples( init.multiSamples, context->device.Features().samples );
	if ( init.multiSamples > 1 && static_cast<int>( samples ) != init.multiSamples ) {
		common->Printf( "d3d12: r_multiSamples %d is not a sample count the adapter has for RGBA8 + D24S8: %u\n", init.multiSamples, samples );
	}
	if ( auto output = context->CreateOutput( static_cast<HWND>( init.nativeWindow ), init.width, init.height, samples ); !output ) {
		return std::unexpected( output.error() );
	}
	context->CreateDefaults();
	context->device.DrainMessages();
	return context;
}

idD3D12Context::idD3D12Context( idD3D12Library && loadedLibrary, idD3D12Device && createdDevice ) :
	library( std::move( loadedLibrary ) ),
	device( std::move( createdDevice ) ),
	liveObjects( device ),
	queue( device ),
	graveyard( queue ),
	commands( device, queue ),
	timers( device, queue ),
	barriers( *this ),
	srvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SHADER_VISIBLE_SRVS ),
	rtvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV ),
	dsvPool( device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV ),
	samplers( device, graveyard ),
	constants( device, CONSTANTS_RING_SIZE, L"doom3 constants ring" ),
	staging( device, STAGING_RING_SIZE, L"doom3 staging ring" ),
	uploader( device, *this, staging, barriers, graveyard ),
	buffers( device, graveyard, uploader, barriers, FramePlacement( device ) ),
	textures( device, graveyard, srvPool, rtvPool, samplers, uploader, barriers, device.Features().unalignedBlocks ),
	hlslCompiler( library, D3D12_GameDataReader(), r_d3d12ShaderCache.GetBool(), r_d3d12ShaderThreads.GetInteger() ),
	shaders( hlslCompiler, graveyard ),
	rootSignature( device, library ),
	computeRootSignature( device, library ),
	journal( LoadJournal() ),
	pipelineLibrary( OpenPipelineLibrary( device ) ),
	pipelines( device, rootSignature, &journal, pipelineLibrary ? &*pipelineLibrary : nullptr ),
	scene( device, graveyard, srvPool, rtvPool, dsvPool ),
	passes( device, rootSignature, hlslCompiler, *this, barriers, uploader, constants, samplers, srvPool ),
	readback( device, *this, barriers, graveyard ),
	copies( *this, barriers, scene, textures, passes, rtvPool ),
	recorder( device, *this, barriers, scene, passes, pipelines, shaders, buffers, textures, constants, samplers ),
	resourcePool( device, graveyard, srvPool, rtvPool ),
	graph( device, *this, barriers, resourcePool, srvPool, rtvPool, constants, computeRootSignature ),
	hdr( device, *this, computeRootSignature, passes, samplers, srvPool ),
	gpuScene( device, *this, graveyard, srvPool, barriers, uploader, textures ),
	rayTracing( device, *this, graveyard, srvPool, barriers, uploader, buffers ),
	rtPasses( device, computeRootSignature, passes, samplers, buffers ) {
}

// Safe after a mid-frame error: the open list is closed without running. The library is written while
// its PSOs live. The members then die in reverse order, the DLLs last.
idD3D12Context::~idD3D12Context() {
	commands.Abandon();
	barriers.Discard();
	if ( !device.IsLost() ) {
		WaitIdle();
	}
	SavePipelines( !device.IsLost() );
	buffers.DestroyAll();
	textures.DestroyAll();
	shaders.DestroyAll();
	resourcePool.FreeAll();
	graveyard.CollectAll();
	device.DrainMessages();
}

// The sample count holds until Shutdown, as the contract says: resizes keep it.
std::expected<void, idStr> idD3D12Context::CreateOutput( HWND window, int width, int height, UINT samples ) {
	auto created = idD3D12SwapChain::Create( device, queue, rtvPool, window, width, height );
	if ( !created ) {
		return std::unexpected( created.error() );
	}
	swapChain.emplace( std::move( *created ) );
	scene.Create( swapChain->Width(), swapChain->Height(), samples );
	return {};
}

void idD3D12Context::CreateDefaults() {
	textures.CreateDefaults();
	passes.CreateDefaults();
	hdr.CreateDefaults();
	rayTracing.CreateDefaults( passes.Stage( "internal_rtSkin" ), computeRootSignature );
	rtPasses.CreateDefaults();
	buffers.CreateDefaults();
	shaders.Prebuild();
}

ID3D12GraphicsCommandList4 & idD3D12Context::List() {
	if ( commands.IsOpen() ) {
		return commands.List();
	}
	ID3D12GraphicsCommandList4 & list = commands.Open();
	// the directly indexed heaps come before the root signature
	ID3D12DescriptorHeap * heaps[] = { srvPool.ShaderVisibleHeap(), samplers.Heap() };
	list.SetDescriptorHeaps( 2, heaps );
	list.SetGraphicsRootSignature( rootSignature.Get() );
	recorder.NewList();
	groups.Reopen( list );
	return list;
}

uint64 idD3D12Context::SubmitChunk() {
	if ( !commands.IsOpen() ) {
		return queue.LastSignaled();
	}
	barriers.Flush();
	groups.CloseAll( commands.List() );
	const uint64 fence = commands.CloseAndExecute();
	constants.CloseChunk( fence );
	staging.CloseChunk( fence );
	frame.submits++;
	return fence;
}

void idD3D12Context::WaitFor( uint64 fence ) {
	if ( fence > queue.Waited() ) {
		frame.waits++;
	}
	queue.Wait( fence );
	Retire( queue.Waited() );
}

// The fence of the last list comes before the Present that still reads the back buffer.
void idD3D12Context::WaitIdle() {
	SubmitChunk();
	WaitFor( queue.Signal() );
}

void idD3D12Context::Retire( uint64 waitedFence ) {
	constants.Retire( waitedFence );
	staging.Retire( waitedFence );
	graveyard.Collect();
}

// Dragging the border resizes the window without ResizeOutput; nothing of the frame is recorded yet,
// so the output can follow here. The frame timer's first stamp then opens the frame's list.
void idD3D12Context::BeginFrame( int width, int height ) {
	if ( swapChain && ( width != swapChain->Width() || height != swapChain->Height() ) ) {
		Resize( width, height );
	}
	recorder.BeginFrame();
	if ( !device.IsLost() ) {
		timers.BeginFrame( List() );
	}
}

// The resolves go into the list submitted here, so the Present that follows waits for them too.
void idD3D12Context::EndFrame() {
	recorder.EndPass();
	if ( !device.IsLost() ) {
		timers.EndFrame( List() );
	}
	SubmitChunk();
}

void idD3D12Context::BlitToBackBuffer( idD3D12SwapChain::backBuffer_t & back ) {
	recorder.EndPass();
	idD3D12SceneTarget::surface_t & source = scene.Resolved( barriers, *this );
	barriers.Transition( *source.resource.Get(), source.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	barriers.Transition( *back.resource.Get(), back.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	passes.PresentBlit( source.srv, rtvPool.Cpu( back.rtv ), scene.Width(), scene.Height() );
	recorder.Invalidate();
}

// Blocks until the GPU has finished the frame: the next frame maps the vertex cache set this one read.
presentResult_t idD3D12Context::Present( presentMode_t mode ) {
	if ( device.IsLost() || !swapChain ) {
		return presentResult_t::DEVICE_LOST;
	}
	// a frame nobody sees stays in the scene target: no blit, and the swap chain keeps its buffer
	if ( mode == presentMode_t::NONE ) {
		WaitFor( SubmitChunk() );
		EndFrameStats();
		return device.IsLost() ? presentResult_t::DEVICE_LOST : presentResult_t::OK;
	}
	idD3D12SwapChain::backBuffer_t & back = swapChain->Current();
	BlitToBackBuffer( back );
	barriers.Transition( *back.resource.Get(), back.state, D3D12_RESOURCE_STATE_PRESENT );
	const uint64 fence = SubmitChunk();

	const presentResult_t result = swapChain->Present( mode );
	WaitFor( fence );
	swapChain->WaitForLatency();
	EndFrameStats();
	return device.IsLost() ? presentResult_t::DEVICE_LOST : result;
}

void idD3D12Context::EndFrameStats() {
	frame.draw = recorder.TakeStats();
	frame.psoCreated = pipelines.CreatedThisFrame();
	frame.nearestBiases = pipelines.Bias().NearestThisFrame();
	frame.barriers = barriers.IssuedThisFrame();
	frame.uploadBytes = uploader.BytesThisFrame();
	frame.inPlaceBytes = buffers.InPlaceBytesThisFrame();
	frame.samplersMade = samplers.BuiltThisFrame();
	frame.debugGroups = groups.PushedThisFrame();
	frame.timestamps = timers.WrittenThisFrame();
	frame.gpuMicroseconds = timers.FrameMicroseconds();
	const idD3D12ScreenCopies::stats_t copied = copies.TakeStats();
	frame.colorCopies = copied.color;
	frame.depthCopies = copied.depth;
	frame.copySurfaces = textures.CopySurfacesThisFrame();
	lastFrame = frame;
	frame = {};
	textures.ResetFrameCounters();
	pipelines.ResetFrameCounters();
	barriers.ResetFrameCounters();
	uploader.ResetFrameCounters();
	buffers.ResetFrameCounters();
	samplers.ResetFrameCounters();
	groups.ResetFrameCounters();
	timers.ResetFrameCounters();
	resourcePool.EndFrame();
}

void idD3D12Context::PushGroup( const char * name ) {
	groups.Push( List(), name );
}

void idD3D12Context::PopGroup() {
	groups.Pop( commands.IsOpen() ? &commands.List() : nullptr );
}

// Over everything the frame drew, in the scene target, so ReadOutputPixels captures it.
void idD3D12Context::ShowImage( textureHandle_t handle ) {
	const d3d12Texture_t * texture = textures.Find( handle );
	if ( texture == nullptr || !swapChain ) {
		return;
	}
	recorder.EndPass();
	textures.MakeReadable( handle );
	idD3D12SceneTarget::surface_t & color = scene.Color();
	barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	passes.ShowImage( textures.SampledView( handle, texture->kind ), texture->kind, texture->surface.width, texture->surface.height,
		scene.Rtv( false ), scene.Target( false ), scene.Width(), scene.Height() );
	recorder.Invalidate();
}

void idD3D12Context::CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	if ( device.IsLost() ) {
		return;
	}
	recorder.EndPass();
	copies.Color( texture, x, y, width, height, hdrScene );
}

// preExposure: the manual exposure, or the auto exposure of the last resolve read back (Present waits for its
// frame, so that is the last frame's); a finite one, else the manual exposure.
void idD3D12Context::BeginHdrScene( const postProcessSettings_t & settings ) {
	if ( device.IsLost() ) {
		return;
	}
	recorder.EndPass();
	scene.EnsureHdr( barriers, *this );
	if ( !scene.Color( true ).resource ) {
		return;
	}
	hdr.Poll();
	preExposure = R_ManualExposure( settings );
	if ( settings.autoExposure && hdr.LastExposure() ) {
		const float last = hdr.LastExposure()->exposure.exposure;
		preExposure = ( last > 1e-6f && last < 1e6f ) ? last : preExposure;
	}
	recorder.SetHdr( true, preExposure );
	hdrScene = true;
}

void idD3D12Context::SetHdrSceneGamma( bool gamma, const renderRect_t & viewport ) {
	if ( !hdrScene || recorder.HdrGamma() == gamma || device.IsLost() ) {
		return;
	}
	recorder.SetHdrGamma( gamma );
	hdr.Encode( graph, scene, gamma, viewport, ( groups.Depth() > 0 ) ? &groups : nullptr );
	recorder.Invalidate();
}

void idD3D12Context::ResolveHdrScene( const postProcessSettings_t & settings, const renderRect_t & viewport, const hdrExposureStep_t & step ) {
	if ( !hdrScene ) {
		return;
	}
	SetHdrSceneGamma( false, viewport );
	EndHdrScene();
	if ( device.IsLost() ) {
		return;
	}
	hdr.Resolve( graph, scene, settings, viewport, preExposure, step, ( groups.Depth() > 0 ) ? &groups : nullptr );
	recorder.Invalidate();
}

std::optional<hdrExposure_t> idD3D12Context::LastHdrExposure() const {
	return hdr.LastExposure() ? std::optional( hdr.LastExposure()->exposure ) : std::nullopt;
}

void idD3D12Context::EndHdrScene() {
	if ( hdrScene ) {
		recorder.SetHdr( false, preExposure );
		hdrScene = false;
	}
}

bool idD3D12Context::BeginGBuffer( const renderRect_t & viewport ) {
	if ( !hdrScene || recorder.HdrGamma() || device.IsLost() ) {
		return false;
	}
	recorder.EndPass();
	if ( !scene.EnsureGBuffer() ) {
		return false;
	}
	const D3D12_RECT rect = { Max( viewport.x, 0 ), Max( viewport.y, 0 ), Min( viewport.x + viewport.width, scene.Width() ),
		Min( viewport.y + viewport.height, scene.Height() ) };
	if ( rect.right <= rect.left || rect.bottom <= rect.top ) {
		return false;
	}
	scene.ClearGBuffer( barriers, List(), rect );
	recorder.SetGBuffer( true );
	return true;
}

void idD3D12Context::EndGBuffer() {
	if ( recorder.GBuffer() ) {
		recorder.SetGBuffer( false );
	}
}

// A failed build leaves nothing to trace this frame; a full update would not help it, so the update stands.
bool idD3D12Context::UpdateGpuScene( const gpuSceneUpdate_t & update, gpuSceneStats_t & stats ) {
	recorder.EndPass();
	const bool applied = gpuScene.Update( update );
	if ( applied ) {
		static_cast<void>( rayTracing.Build( graph, gpuScene, ( groups.Depth() > 0 ) ? &groups : nullptr, stats ) );
	} else {
		rayTracing.Invalidate();
		stats = {};
	}
	recorder.Invalidate();
	return applied;
}

void idD3D12Context::DrawRayTracingDebug( rtDebug_t mode, const rtView_t & view, const renderRect_t & viewport ) {
	if ( mode == rtDebug_t::NONE || device.IsLost() ) {
		return;
	}
	recorder.EndPass();
	rtPasses.Debug( graph, scene, rayTracing, gpuScene, mode, view, viewport, ( groups.Depth() > 0 ) ? &groups : nullptr );
	recorder.Invalidate();
}

void idD3D12Context::DrawRayTracedReflections( const rtReflectionSettings_t & settings, const rtView_t & view, const renderRect_t & viewport ) {
	if ( !settings.enabled || !hdrScene || recorder.HdrGamma() || device.IsLost() ) {
		return;
	}
	recorder.EndPass();
	rtPasses.Reflections( graph, scene, rayTracing, gpuScene, settings, view, viewport, preExposure, ( groups.Depth() > 0 ) ? &groups : nullptr );
	recorder.Invalidate();
}

void idD3D12Context::DrawGBufferDebug( gbufferDebug_t mode, const renderRect_t & viewport ) {
	if ( mode == gbufferDebug_t::NONE || device.IsLost() || !scene.GBuffer( 0 ).resource ) {
		return;
	}
	recorder.EndPass();
	std::array<d3d12Descriptor_t, idD3D12SceneTarget::GBUFFER_TARGETS> views;
	for ( size_t i = 0; i < views.size(); i++ ) {
		idD3D12SceneTarget::surface_t & target = scene.GBuffer( i );
		barriers.Transition( *target.resource.Get(), target.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		views[i] = target.srv;
	}
	idD3D12SceneTarget::surface_t & color = scene.Color();
	barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	passes.GBufferDebug( views, scene.Samples(), mode, scene.Rtv( false ), scene.Target( false ), viewport );
	recorder.Invalidate();
}

void idD3D12Context::CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	if ( device.IsLost() ) {
		return;
	}
	recorder.EndPass();
	copies.Depth( texture, x, y, width, height );
	recorder.Invalidate();
}

d3d12TextureCheckResult_t idD3D12Context::CheckTextures() {
	recorder.EndPass();
	idD3D12TextureCheck check( device, *this, barriers, graveyard );
	return check.Run( textures );
}

// r_d3d12CaptureBackBuffer reads what Present would show, so a wrong row flip in the blit makes the
// capture differ from the scene's; Present then blits into the same back buffer again.
void idD3D12Context::ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) {
	recorder.EndPass();
	if ( r_d3d12CaptureBackBuffer.GetBool() && swapChain ) {
		idD3D12SwapChain::backBuffer_t & back = swapChain->Current();
		BlitToBackBuffer( back );
		readback.Read( *back.resource.Get(), back.state, x, y, width, height, rgb, true );
		return;
	}
	idD3D12SceneTarget::surface_t & source = scene.Resolved( barriers, *this );
	readback.Read( *source.resource.Get(), source.state, x, y, width, height, rgb, false );
}

void idD3D12Context::Resize( int width, int height ) {
	if ( width <= 0 || height <= 0 || !swapChain ) {
		return;
	}
	if ( width == swapChain->Width() && height == swapChain->Height() ) {
		return;
	}
	WaitIdle();
	swapChain->Resize( width, height );
	scene.Create( swapChain->Width(), swapChain->Height(), scene.Samples() );
	recorder.Invalidate();
	hdr.ResetExposure();
	// the old scene target dies after the next wait: a leak shows as memory that grows from resize to resize
	const d3d12VideoMemory_t memory = device.VideoMemory();
	common->DPrintf( "d3d12: output resized to %d x %d; %d graves, %.1f MB of video memory in use\n", width, height, graveyard.Pending(),
		static_cast<double>( memory.used ) / ( 1024.0 * 1024.0 ) );
}

void idD3D12Context::SetSrgb( bool enable ) {
	recorder.SetSrgb( enable );
}

void idD3D12Context::SetGamma( const d3d12GammaRamp_t & ramp ) {
	if ( !device.IsLost() ) {
		passes.SetGamma( ramp );
	}
}

// Built-in programs at Init, material programs at a level load: both before a frame draws with them.
programHandle_t idD3D12Context::CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * name, programInfo_t & info ) {
	const programHandle_t handle = shaders.CreateProgram( vertex, fragment, name, info );
	if ( handle != programHandle_t::NONE && !device.IsLost() ) {
		pipelines.Warm( shaders.Program( handle ), scene.Samples() );
	}
	return handle;
}

// The journal is written here too, so a run that crashes later still warms the next one.
void idD3D12Context::EndLevelLoad() {
	const idD3D12PipelineFactory::stats_t level = pipelines.TakeLevelStats();
	common->Printf( "d3d12 pipelines: %d warmed for the level in %.1f ms; %d made by draws since the last load in %.1f ms; %d of both from the library\n",
		level.warmed, static_cast<double>( level.warmMicroseconds ) / 1000.0, level.drawn, static_cast<double>( level.drawnMicroseconds ) / 1000.0,
		level.loaded );
	SavePipelines( false );
	hdr.ResetExposure();
}

// The file system outlives the renderer (common's Shutdown), but not a failed Init.
void idD3D12Context::SavePipelines( bool withLibrary ) {
	if ( fileSystem == nullptr || !fileSystem->IsInitialized() ) {
		return;
	}
	if ( journal.Dirty() && WriteSaved( JOURNAL_FILE, journal.Write() ) ) {
		journal.MarkSaved();
	}
	if ( withLibrary && pipelineLibrary && pipelineLibrary->Dirty() && WriteSaved( LIBRARY_FILE, pipelineLibrary->Write() ) ) {
		pipelineLibrary->MarkSaved();
	}
}

void idD3D12Context::PrintInfo() const {
	device.PrintInfo();
	const auto Version = []( uint64 version ) {
		return idStr( va( "%u.%u.%u.%u", static_cast<unsigned>( version >> 48 ), static_cast<unsigned>( ( version >> 32 ) & 0xFFFF ),
			static_cast<unsigned>( ( version >> 16 ) & 0xFFFF ), static_cast<unsigned>( version & 0xFFFF ) ) );
	};
	common->Printf( "D3D12 shader compiler: dxcompiler.dll %s compiled %d, %d from the cache in %.1f ms; %d stages, %d programs\n",
		Version( library.dxcVersion ).c_str(), hlslCompiler.Compiled(), hlslCompiler.CacheHits(), hlslCompiler.Milliseconds(), shaders.NumStages(),
		shaders.NumPrograms() );
	const idD3D12PipelineFactory::stats_t & made = pipelines.TotalStats();
	common->Printf( "D3D12 pipelines: %d PSOs, %d warmed from the journal and %d made by draws, %d from the library; %d depth biases; depth bounds %s\n",
		pipelines.Num(), made.warmed, made.drawn, made.loaded, pipelines.Bias().Num(), device.Features().depthBounds ? "in every PSO" : "unsupported" );
	common->Printf( "D3D12 pipeline journal: %d keys of %d programs; library %s\n", journal.NumEntries(), journal.NumPrograms(),
		!pipelineLibrary ? "off" : pipelineLibrary->FromFile() ? va( "opened over %.1f MB", Megabytes( pipelineLibrary->FileBytes() ) ) : "new" );
	common->Printf( "D3D12 GPU timers: %s, %.3f MHz; the frame timer and %d x %d r_pix slots\n", timers.Available() ? "on" : "off",
		static_cast<double>( timers.Frequency() ) / 1.0e6, TIMESTAMP_SETS, idD3D12GpuTimers::SLOTS_PER_SET );
	if ( swapChain ) {
		common->Printf( "D3D12 swap chain: %d x %d, %u flip-discard buffers\n", swapChain->Width(), swapChain->Height(), idD3D12SwapChain::BUFFERS );
	}
	common->Printf( "D3D12 scene target: %d x %d, RGBA8 + D24S8, %u sample(s), %s writes, gamma %s; constants ring %llu MB, staging ring %llu MB\n",
		scene.Width(), scene.Height(), scene.Samples(), recorder.Srgb() ? "sRGB" : "UNORM", passes.GammaIsIdentity() ? "identity" : "through the LUT",
		static_cast<unsigned long long>( constants.Size() >> 20 ), static_cast<unsigned long long>( staging.Size() >> 20 ) );
	common->Printf( "D3D12 buffers: %d, FRAME %.1f MB in %s, STATIC %.1f MB in DEFAULT heaps; %.1f MB CPU mirrors\n", buffers.Num(),
		Megabytes( buffers.Bytes( bufferUsage_t::FRAME ) ), D3D12_PlacementName( buffers.FramePlacement() ), Megabytes( buffers.Bytes( bufferUsage_t::STATIC ) ),
		Megabytes( buffers.MirrorBytes() ) );
	common->Printf( "D3D12 render graph: %d pooled resources, %.1f MB\n", resourcePool.Num(), Megabytes( resourcePool.Bytes() ) );
	common->Printf( "D3D12 textures: %d, %.1f MB; shader-visible SRVs %d of %u, samplers %d of %u; unaligned BC %s, anisotropic point mips %s\n",
		textures.Num(), textures.Bytes() / ( 1024.0 * 1024.0 ), srvPool.Used(), SHADER_VISIBLE_SRVS, samplers.Used(), idD3D12SamplerHeap::SIZE,
		device.Features().unalignedBlocks ? "yes" : "decoded",
		device.Features().anisoPointMip ? "yes" : "no" );
}
