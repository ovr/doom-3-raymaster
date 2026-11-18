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

#include "../tr_local.h"
#include "d3d12_Backend.h"

static idRenderBackendFactory d3d12Factory( "d3d12", []() -> idRenderBackend * { return new idRenderBackendD3D12; } );

namespace {

struct unimplementedCall_t {
	const char *	name;
	int				calls;
};
// constant-initialized: no constructor runs before main
std::array<unimplementedCall_t, 64>	unimplementedCalls = {};
int									numUnimplemented = 0;

}	// namespace

void D3D12_Unimplemented( const char * what ) {
	for ( int i = 0; i < numUnimplemented; i++ ) {
		unimplementedCall_t & call = unimplementedCalls[i];
		if ( call.name == what || idStr::Cmp( call.name, what ) == 0 ) {
			call.calls++;
			return;
		}
	}
	if ( numUnimplemented < static_cast<int>( unimplementedCalls.size() ) ) {
		unimplementedCalls[numUnimplemented++] = { what, 1 };
	}
	common->Warning( "d3d12: %s is not implemented yet", what );
}

void D3D12_PrintUnimplemented() {
	if ( numUnimplemented == 0 ) {
		return;
	}
	idStr line = "d3d12: not implemented yet (calls):";
	for ( int i = 0; i < numUnimplemented; i++ ) {
		line += va( " %s %d%s", unimplementedCalls[i].name, unimplementedCalls[i].calls, ( i + 1 < numUnimplemented ) ? "," : "" );
	}
	common->Printf( "%s\n", line.c_str() );
}

idRenderBackendD3D12::idRenderBackendD3D12() {
	ownerThread = Sys_GetCurrentThreadID();
}

// False = no device or no output with these parms; nothing is left behind and R_SetNewMode may try again.
bool idRenderBackendD3D12::Init( const renderBackendInit_t & init ) {
	assert( !context );
	ownerThread = Sys_GetCurrentThreadID();
	if ( init.stereoPixelFormat ) {
		common->Printf( "d3d12: no quad-buffer stereo\n" );
		return false;
	}
	auto created = idD3D12Context::Create( init );
	if ( !created ) {
		common->Warning( "d3d12: Init failed: %s", created.error().c_str() );
		return false;
	}
	context = std::move( *created );
	context->device.FillCaps( caps );
	caps.gpuTimer = context->timers.Available();
	caps.timestamps = context->timers.Available();
	context->textures.RecordUploads( r_d3d12CheckImages.GetBool() );
	D3D12_SetActiveContext( context.get() );
	outputWidth = init.width;
	outputHeight = init.height;

	const d3d12Features_t & features = context->device.Features();
	common->Printf( "d3d12: adapter %s, %llu MB, %s, debug level %d\n", features.adapterName.c_str(),
		static_cast<unsigned long long>( features.dedicatedVideoMemory >> 20 ), features.versionString.c_str(), context->device.DebugLevel() );
	return true;
}

void idRenderBackendD3D12::Shutdown() {
	if ( context ) {
		D3D12_PrintUnimplemented();
		common->Printf( "d3d12 shaders: compiled %d, %d from the cache\n", context->compiler.Compiled(), context->compiler.CacheHits() );
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

// Cube maps are always seamless in D3D12, and the sample count is fixed at Init.
void idRenderBackendD3D12::SetOption( renderOption_t option, bool enable ) {
	if ( option == renderOption_t::SRGB_OUTPUT && enable ) {
		D3D12_Unimplemented( "SetOption( SRGB_OUTPUT )" );
	}
}

void idRenderBackendD3D12::SetGammaRamp( const unsigned short [256], const unsigned short [256], const unsigned short [256] ) {
	D3D12_Unimplemented( "SetGammaRamp" );
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
	D3D12_PrintUnimplemented();
}

void idRenderBackendD3D12::BeginFrame() {
	assert( !inFrame );
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

// Present has waited for the frame, so unlike GL's query this never blocks.
uint64 idRenderBackendD3D12::GetFrameGpuMicroseconds() {
	return context ? context->timers.FrameMicroseconds() : 0;
}

void idRenderBackendD3D12::SelectOutput( outputEye_t ) {
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
	if ( context && r_d3d12CheckImages.GetBool() && !context->device.IsLost() ) {
		static_cast<void>( context->CheckTextures() );
	}
}

void idRenderBackendD3D12::CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	context->CopyOutputToTexture( texture, x, y, width, height );
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
	return context->shaders.CreateProgram( vertex, fragment, debugName, info );
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
