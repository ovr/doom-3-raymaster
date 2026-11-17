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

bufferHandle_t idD3D12Placeholders::CreateBuffer( int size, const void * data ) {
	auto record = std::make_unique<buffer_t>();
	record->size = Max( size, 16 );
	record->memory.reset( static_cast<byte *>( Mem_ClearedAlloc( static_cast<size_t>( record->size ), TAG_RENDER ) ) );
	if ( data != nullptr && size > 0 ) {
		memcpy( record->memory.get(), data, static_cast<size_t>( size ) );
	}
	const bufferHandle_t handle = static_cast<bufferHandle_t>( reinterpret_cast<uintptr_t>( record.get() ) );
	buffers.push_back( std::move( record ) );
	return handle;
}

// A handle from before the last Clear is no record any more and is ignored.
void idD3D12Placeholders::DestroyBuffer( bufferHandle_t buffer ) {
	const auto found = std::find_if( buffers.begin(), buffers.end(), [buffer]( const std::unique_ptr<buffer_t> & record ) {
		return reinterpret_cast<uintptr_t>( record.get() ) == static_cast<uintptr_t>( buffer );
	} );
	if ( found != buffers.end() ) {
		buffers.erase( found );
	}
}

void * idD3D12Placeholders::MapBuffer( bufferHandle_t buffer ) {
	return reinterpret_cast<buffer_t *>( static_cast<uintptr_t>( buffer ) )->memory.get();
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
		context.reset();
	}
	placeholders.Clear();
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
	context->BeginFrame( renderSystem->GetWidth(), renderSystem->GetHeight() );
}

void idRenderBackendD3D12::EndFrame() {
	assert( inFrame );
	context->EndFrame();
	inFrame = false;
}

presentResult_t idRenderBackendD3D12::Present( presentMode_t mode ) {
	return context->Present( mode );
}

uint64 idRenderBackendD3D12::GetFrameGpuMicroseconds() {
	D3D12_Unimplemented( "GetFrameGpuMicroseconds" );
	return 0;
}

void idRenderBackendD3D12::SelectOutput( outputEye_t ) {
}

// Silently black captures would pass for frames, so a read stops the game until it is written.
void idRenderBackendD3D12::ReadOutputPixels( int, int, int, int, byte * ) {
	common->FatalError( "d3d12: ReadOutputPixels is not implemented yet (run the visual demos with +set vdemo_capture 0)" );
}

textureHandle_t idRenderBackendD3D12::CreateTexture( const idImageOpts &, textureRole_t, const samplerDesc_t &, const char * ) {
	D3D12_Unimplemented( "CreateTexture" );
	return placeholders.NewHandle<textureHandle_t>();
}

void idRenderBackendD3D12::UploadTexture( textureHandle_t, int, int, int, int, const void *, int ) {
	D3D12_Unimplemented( "UploadTexture" );
}

void idRenderBackendD3D12::SetTextureSampler( textureHandle_t, const samplerDesc_t & ) {
	D3D12_Unimplemented( "SetTextureSampler" );
}

void idRenderBackendD3D12::CopyOutputToTexture( textureHandle_t, int, int, int, int ) {
	D3D12_Unimplemented( "CopyOutputToTexture" );
}

void idRenderBackendD3D12::CopyDepthToTexture( textureHandle_t, int, int, int, int ) {
	D3D12_Unimplemented( "CopyDepthToTexture" );
}

void idRenderBackendD3D12::API_DestroyTexture( textureHandle_t ) {
	D3D12_Unimplemented( "DestroyTexture" );
}

bufferHandle_t idRenderBackendD3D12::CreateBuffer( bufferType_t, bufferUsage_t, int size, const void * data ) {
	D3D12_Unimplemented( "CreateBuffer" );
	return placeholders.CreateBuffer( size, data );
}

void idRenderBackendD3D12::DestroyBuffer( bufferHandle_t buffer ) {
	placeholders.DestroyBuffer( buffer );
}

void * idRenderBackendD3D12::MapBuffer( bufferHandle_t buffer ) {
	return idD3D12Placeholders::MapBuffer( buffer );
}

void idRenderBackendD3D12::UnmapBuffer( bufferHandle_t, int, int ) {
	D3D12_Unimplemented( "UnmapBuffer" );
}

shaderHandle_t idRenderBackendD3D12::CreateShaderStage( shaderStageType_t, const char *, shaderStageInfo_t & info ) {
	D3D12_Unimplemented( "CreateShaderStage" );
	info.numParms = 0;
	return placeholders.NewHandle<shaderHandle_t>();
}

programHandle_t idRenderBackendD3D12::CreateProgram( shaderHandle_t, shaderHandle_t, const char *, programInfo_t & info ) {
	D3D12_Unimplemented( "CreateProgram" );
	info = {};
	return placeholders.NewHandle<programHandle_t>();
}

void idRenderBackendD3D12::DestroyShaderStage( shaderHandle_t ) {
	D3D12_Unimplemented( "DestroyShaderStage" );
}

void idRenderBackendD3D12::DestroyProgram( programHandle_t ) {
	D3D12_Unimplemented( "DestroyProgram" );
}

// caps.timestamps and caps.debugGroups are false: shared code does not call these.
void idRenderBackendD3D12::WriteTimestamp( int, int ) {
	D3D12_Unimplemented( "WriteTimestamp" );
}

bool idRenderBackendD3D12::TimestampReady( int, int ) {
	D3D12_Unimplemented( "TimestampReady" );
	return false;
}

void idRenderBackendD3D12::ReadTimestamps( int, int count, uint64 * nanoseconds ) {
	D3D12_Unimplemented( "ReadTimestamps" );
	std::fill_n( nanoseconds, Max( count, 0 ), uint64( 0 ) );
}

void idRenderBackendD3D12::FreeTimestamps() {
}

void idRenderBackendD3D12::PushDebugGroup( const char * ) {
	D3D12_Unimplemented( "PushDebugGroup" );
}

void idRenderBackendD3D12::PopDebugGroup() {
	D3D12_Unimplemented( "PopDebugGroup" );
}

void idRenderBackendD3D12::API_Clear( const clearDesc_t & ) {
	D3D12_Unimplemented( "Clear" );
}

// Counted and skipped.
void idRenderBackendD3D12::API_DrawIndexed( const drawCall_t & ) {
	D3D12_Unimplemented( "DrawIndexed" );
}
