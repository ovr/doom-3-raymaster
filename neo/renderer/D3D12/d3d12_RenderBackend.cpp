// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Backend.h"

idRenderBackend * R_CreateRenderBackend() {
	return new idRenderBackendD3D12;
}

idRenderBackendD3D12::idRenderBackendD3D12() {
	ownerThread = Sys_GetCurrentThreadID();
}

std::expected<void, idStr> idRenderBackendD3D12::Init( const renderBackendInit_t & init ) {
	assert( !context );
	ownerThread = Sys_GetCurrentThreadID();
	auto created = idD3D12Context::Create( init );
	if ( !created ) {
		return std::unexpected( created.error() );
	}
	context = std::move( *created );
	context->device.FillCaps( caps );
	caps.gpuTimer = context->timers.Available();
	caps.timestamps = context->timers.Available();
	caps.hdrScene = true;
	context->textures.RecordUploads( r_d3d12CheckImages.GetBool() );
	context->SetSrgb( srgbOutput );
	context->SetGamma( gamma );
	D3D12_SetActiveContext( context.get() );
	outputWidth = init.width;
	outputHeight = init.height;

	const d3d12Features_t & features = context->device.Features();
	common->Printf( "d3d12: adapter %s, %llu MB, %s, debug level %d, %u sample(s)\n", features.adapterName.c_str(),
		static_cast<unsigned long long>( features.dedicatedVideoMemory >> 20 ), features.versionString.c_str(), context->device.DebugLevel(),
		context->scene.Samples() );
	return {};
}

void idRenderBackendD3D12::Shutdown() {
	if ( context ) {
		const idD3D12PipelineFactory::stats_t & made = context->pipelines.TotalStats();
		common->Printf( "d3d12 shaders: compiled %d, %d from the cache in %.1f ms; PSOs: %d warmed from the journal in %.1f ms, %d made by draws "
			"in %.1f ms, %d of both from the library\n", context->hlslCompiler.Compiled(), context->hlslCompiler.CacheHits(),
			context->hlslCompiler.Milliseconds(), made.warmed,
			static_cast<double>( made.warmMicroseconds ) / 1000.0, made.drawn, static_cast<double>( made.drawnMicroseconds ) / 1000.0, made.loaded );
		D3D12_SetActiveContext( nullptr );
		context.reset();
	}
	report.Reset();
	viewer.Reset();
	inFrame = false;
}

void idRenderBackendD3D12::ResizeOutput( int width, int height ) {
	outputWidth = width;
	outputHeight = height;
	if ( context ) {
		context->Resize( width, height );
	}
}

// The setting is kept for the next Init.
void idRenderBackendD3D12::SetOption( renderOption_t option, bool enable ) {
	if ( option != renderOption_t::SRGB_OUTPUT || enable == srgbOutput ) {
		return;
	}
	common->Printf( "d3d12: the scene is written through its %s view\n", enable ? "sRGB" : "UNORM" );
	srgbOutput = enable;
	if ( context ) {
		context->SetSrgb( enable );
	}
}

// The window shows the ramp through the present blit, never the desktop's hardware ramp (O14).
void idRenderBackendD3D12::SetGammaRamp( const unsigned short red[256], const unsigned short green[256], const unsigned short blue[256] ) {
	std::copy_n( red, 256, gamma.red.begin() );
	std::copy_n( green, 256, gamma.green.begin() );
	std::copy_n( blue, 256, gamma.blue.begin() );
	if ( context ) {
		context->SetGamma( gamma );
	}
}

void idRenderBackendD3D12::CheckErrors() {
	if ( !context ) {
		return;
	}
	context->device.DrainMessages();
	if ( context->device.IsLost() ) {
		context->device.ReportLoss();
	}
}

void idRenderBackendD3D12::PrintInfo() const {
	if ( context ) {
		context->PrintInfo();
	}
}

void idRenderBackendD3D12::BeginFrame() {
	assert( !inFrame );
	context->shaders.DropPrebuilt();
	inFrame = true;
	context->textures.RecordUploads( r_d3d12CheckImages.GetBool() );
	context->BeginFrame( renderSystem->GetWidth(), renderSystem->GetHeight() );
}

void idRenderBackendD3D12::EndFrame() {
	assert( inFrame );
	viewer.Frame( *context );
	context->EndFrame();
	inFrame = false;
}

presentResult_t idRenderBackendD3D12::Present( presentMode_t mode ) {
	const presentResult_t result = context->Present( mode );
	report.Frame( *context );
	return result;
}

// Present has waited for the frame, so this never blocks.
uint64 idRenderBackendD3D12::GetFrameGpuMicroseconds() {
	return context ? context->timers.FrameMicroseconds() : 0;
}

// A lost device reads black.
void idRenderBackendD3D12::ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) {
	if ( !context || context->device.IsLost() ) {
		memset( rgb, 0, static_cast<size_t>( Max( width, 0 ) ) * static_cast<size_t>( Max( height, 0 ) ) * 3 );
		return;
	}
	context->ReadOutputPixels( x, y, width, height, rgb );
}

// Legal at any time (R2); before Init and after Shutdown there is no texture.
textureHandle_t idRenderBackendD3D12::CreateTexture( const idImageOpts & opts, textureRole_t role, const samplerDesc_t & sampler, const char * debugName ) {
	return context ? context->textures.Create( opts, role, sampler, debugName ) : textureHandle_t::NONE;
}

void idRenderBackendD3D12::UploadTexture( textureHandle_t texture, int mipLevel, int face, int width, int height, const void * pixels, int pixelPitch ) {
	if ( context ) {
		context->textures.Upload( texture, mipLevel, face, width, height, pixels, pixelPitch );
	}
}

void idRenderBackendD3D12::SetTextureSampler( textureHandle_t texture, const samplerDesc_t & sampler ) {
	if ( context ) {
		context->textures.SetSampler( texture, sampler );
	}
}

// Main thread, between frames: the check submits and waits.
void idRenderBackendD3D12::EndLevelLoad() {
	if ( !context ) {
		return;
	}
	if ( r_d3d12CheckImages.GetBool() && !context->device.IsLost() ) {
		static_cast<void>( context->CheckTextures() );
	}
	context->EndLevelLoad();
}

void idRenderBackendD3D12::CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	context->CopyOutputToTexture( texture, x, y, width, height );
}

void idRenderBackendD3D12::BeginHdrScene( const postProcessSettings_t & settings ) {
	context->BeginHdrScene( settings );
}

void idRenderBackendD3D12::ResolveHdrScene( const postProcessSettings_t & settings, const renderRect_t & viewport, const hdrExposureStep_t & step ) {
	context->ResolveHdrScene( settings, viewport, step );
}

void idRenderBackendD3D12::EndHdrScene() {
	context->EndHdrScene();
}

void idRenderBackendD3D12::SetHdrSceneGamma( bool encoded, const renderRect_t & viewport ) {
	context->SetHdrSceneGamma( encoded, viewport );
}

bool idRenderBackendD3D12::BeginGBuffer( const renderRect_t & viewport ) {
	return context->BeginGBuffer( viewport );
}

void idRenderBackendD3D12::EndGBuffer() {
	context->EndGBuffer();
}

bool idRenderBackendD3D12::BeginTracedLights( const renderRect_t & rect ) {
	assert( IsOwnerThread() && inFrame );
	return context->BeginTracedLights( rect );
}

void idRenderBackendD3D12::EndTracedLights() {
	context->EndTracedLights();
}

void idRenderBackendD3D12::DrawGBufferDebug( gbufferDebug_t mode, const renderRect_t & viewport ) {
	context->DrawGBufferDebug( mode, viewport );
}

bool idRenderBackendD3D12::UpdateGpuScene( const gpuSceneUpdate_t & update, gpuSceneStats_t & stats ) {
	assert( IsOwnerThread() && inFrame );
	return context->UpdateGpuScene( update, stats );
}

void idRenderBackendD3D12::DrawRayTracingDebug( rtDebug_t mode, const rtView_t & view, const renderRect_t & viewport ) {
	assert( IsOwnerThread() && inFrame );
	context->DrawRayTracingDebug( mode, view, viewport );
}

void idRenderBackendD3D12::DrawRayTracedReflections( const rtReflectionSettings_t & settings, const rtView_t & view, const renderRect_t & viewport ) {
	assert( IsOwnerThread() && inFrame );
	context->DrawRayTracedReflections( settings, view, viewport );
}

std::optional<hdrExposure_t> idRenderBackendD3D12::LastHdrExposure() const {
	return context ? context->LastHdrExposure() : std::nullopt;
}

void idRenderBackendD3D12::CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	context->CopyDepthToTexture( texture, x, y, width, height );
}

// After Shutdown there is nothing to destroy: every older handle is invalid (contract).
void idRenderBackendD3D12::API_DestroyTexture( textureHandle_t texture ) {
	if ( context ) {
		context->textures.Destroy( texture );
	}
}

bufferHandle_t idRenderBackendD3D12::CreateBuffer( bufferType_t type, bufferUsage_t usage, int size, const void * data ) {
	return context ? context->buffers.Create( type, usage, size, data ) : bufferHandle_t::NONE;
}

// After Shutdown there is nothing to destroy: every older handle is invalid (contract).
void idRenderBackendD3D12::DestroyBuffer( bufferHandle_t buffer ) {
	if ( context ) {
		context->recorder.ForgetBuffer( buffer );
		context->buffers.Destroy( buffer );
	}
}

// Any thread (R1): a lookup of a live record, no API call.
void * idRenderBackendD3D12::MapBuffer( bufferHandle_t buffer ) {
	return context->buffers.Map( buffer );
}

void idRenderBackendD3D12::UnmapBuffer( bufferHandle_t buffer, int dirtyOffset, int dirtyBytes ) {
	context->buffers.Unmap( buffer, dirtyOffset, dirtyBytes );
}

// Legal at any time (R2), like a texture; before Init and after Shutdown there is no stage.
shaderHandle_t idRenderBackendD3D12::CreateShaderStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info ) {
	if ( !context ) {
		info.numParms = 0;
		return shaderHandle_t::NONE;
	}
	return context->shaders.CreateStage( stage, name, info );
}

programHandle_t idRenderBackendD3D12::CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * debugName, programInfo_t & info ) {
	if ( !context ) {
		info = {};
		return programHandle_t::NONE;
	}
	return context->CreateProgram( vertex, fragment, debugName, info );
}

void idRenderBackendD3D12::DestroyShaderStage( shaderHandle_t shader ) {
	if ( context ) {
		context->shaders.DestroyStage( shader );
	}
}

// R3: the latch forgets it at once, as GL's does.
void idRenderBackendD3D12::DestroyProgram( programHandle_t program ) {
	if ( latch.program == program ) {
		latch.program = programHandle_t::NONE;
	}
	if ( context ) {
		context->recorder.ForgetProgram( program );
		context->shaders.DestroyProgram( program );
	}
}

void idRenderBackendD3D12::WriteTimestamp( int set, int index ) {
	assert( inFrame );
	if ( !context->device.IsLost() ) {
		context->timers.Write( context->List(), set, index );
	}
}

bool idRenderBackendD3D12::TimestampReady( int set, int index ) {
	return context && context->timers.Ready( set, index );
}

void idRenderBackendD3D12::ReadTimestamps( int set, int count, uint64 * nanoseconds ) {
	const std::span<uint64> slots( nanoseconds, static_cast<size_t>( Max( count, 0 ) ) );
	if ( context ) {
		context->timers.Read( set, slots );
	} else {
		std::ranges::fill( slots, uint64( 0 ) );
	}
}

// RenderLog frees them before Shutdown; after it there is nothing left to free.
void idRenderBackendD3D12::FreeTimestamps() {
	if ( context ) {
		context->timers.Free();
	}
}

void idRenderBackendD3D12::PushDebugGroup( const char * name ) {
	context->PushGroup( name );
}

void idRenderBackendD3D12::PopDebugGroup() {
	context->PopGroup();
}

void idRenderBackendD3D12::API_Clear( const clearDesc_t & clear ) {
	context->recorder.Clear( latch, clear );
}

// R12: a draw without a program is counted and skipped.
void idRenderBackendD3D12::API_DrawIndexed( const drawCall_t & dc ) {
	if ( latch.program == programHandle_t::NONE ) {
		context->recorder.SkippedDraw();
		return;
	}
	context->recorder.Draw( latch, PipelineKey( dc ), dc );
}
