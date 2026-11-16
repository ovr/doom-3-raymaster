/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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
#include "gl_Backend.h"

static idRenderBackendFactory glFactory( "opengl", []() -> idRenderBackend * { return new idRenderBackendGL; } );

/*
========================
NotMigrated

The body of this method still lives in the old OpenGL path and nothing may call it yet.
========================
*/
static void NotMigrated( const char * method ) {
	common->FatalError( "idRenderBackendGL::%s is not migrated yet", method );
}

/*
========================
idRenderBackendGL::idRenderBackendGL
========================
*/
idRenderBackendGL::idRenderBackendGL() {
	ownerThread = Sys_GetCurrentThreadID();
}

/*
========================
idRenderBackendGL::Init
========================
*/
bool idRenderBackendGL::Init( const renderBackendInit_t & init ) {
	NotMigrated( __func__ );
	return false;
}

/*
========================
idRenderBackendGL::Shutdown
========================
*/
void idRenderBackendGL::Shutdown() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::ResizeOutput
========================
*/
void idRenderBackendGL::ResizeOutput( int width, int height ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::SetOption
========================
*/
void idRenderBackendGL::SetOption( renderOption_t option, bool enable ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::SetGammaRamp
========================
*/
void idRenderBackendGL::SetGammaRamp( const unsigned short red[256], const unsigned short green[256], const unsigned short blue[256] ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::CheckErrors
========================
*/
void idRenderBackendGL::CheckErrors() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::PrintInfo
========================
*/
void idRenderBackendGL::PrintInfo() const {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::BeginFrame
========================
*/
void idRenderBackendGL::BeginFrame() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::EndFrame
========================
*/
void idRenderBackendGL::EndFrame() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::Present
========================
*/
presentResult_t idRenderBackendGL::Present( presentMode_t mode ) {
	NotMigrated( __func__ );
	return presentResult_t::OK;
}

/*
========================
idRenderBackendGL::GetFrameGpuMicroseconds
========================
*/
uint64 idRenderBackendGL::GetFrameGpuMicroseconds() {
	NotMigrated( __func__ );
	return 0;
}

/*
========================
idRenderBackendGL::SelectOutput
========================
*/
void idRenderBackendGL::SelectOutput( outputEye_t eye ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::ReadOutputPixels
========================
*/
void idRenderBackendGL::ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::CreateTexture
========================
*/
textureHandle_t idRenderBackendGL::CreateTexture( const idImageOpts & opts, textureRole_t role, const samplerDesc_t & sampler, const char * debugName ) {
	NotMigrated( __func__ );
	return textureHandle_t::NONE;
}

/*
========================
idRenderBackendGL::UploadTexture
========================
*/
void idRenderBackendGL::UploadTexture( textureHandle_t texture, int mipLevel, int face, const void * pixels, int pixelPitch ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::SetTextureSampler
========================
*/
void idRenderBackendGL::SetTextureSampler( textureHandle_t texture, const samplerDesc_t & sampler ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::CopyOutputToTexture
========================
*/
void idRenderBackendGL::CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::CopyDepthToTexture
========================
*/
void idRenderBackendGL::CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::CreateBuffer
========================
*/
bufferHandle_t idRenderBackendGL::CreateBuffer( bufferType_t type, bufferUsage_t usage, int size, const void * data ) {
	NotMigrated( __func__ );
	return bufferHandle_t::NONE;
}

/*
========================
idRenderBackendGL::DestroyBuffer
========================
*/
void idRenderBackendGL::DestroyBuffer( bufferHandle_t buffer ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::MapBuffer
========================
*/
void * idRenderBackendGL::MapBuffer( bufferHandle_t buffer ) {
	NotMigrated( __func__ );
	return nullptr;
}

/*
========================
idRenderBackendGL::UnmapBuffer
========================
*/
void idRenderBackendGL::UnmapBuffer( bufferHandle_t buffer, int dirtyOffset, int dirtyBytes ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::CreateShaderStage
========================
*/
shaderHandle_t idRenderBackendGL::CreateShaderStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info ) {
	NotMigrated( __func__ );
	return shaderHandle_t::NONE;
}

/*
========================
idRenderBackendGL::CreateProgram
========================
*/
programHandle_t idRenderBackendGL::CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * debugName, programInfo_t & info ) {
	NotMigrated( __func__ );
	return programHandle_t::NONE;
}

/*
========================
idRenderBackendGL::DestroyShaderStage
========================
*/
void idRenderBackendGL::DestroyShaderStage( shaderHandle_t shader ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::DestroyProgram
========================
*/
void idRenderBackendGL::DestroyProgram( programHandle_t program ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::WriteTimestamp
========================
*/
void idRenderBackendGL::WriteTimestamp( int set, int index ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::TimestampReady
========================
*/
bool idRenderBackendGL::TimestampReady( int set, int index ) {
	NotMigrated( __func__ );
	return false;
}

/*
========================
idRenderBackendGL::ReadTimestamps
========================
*/
void idRenderBackendGL::ReadTimestamps( int set, int count, uint64 * nanoseconds ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::FreeTimestamps
========================
*/
void idRenderBackendGL::FreeTimestamps() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::PushDebugGroup
========================
*/
void idRenderBackendGL::PushDebugGroup( const char * name ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::PopDebugGroup
========================
*/
void idRenderBackendGL::PopDebugGroup() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_SetDefaultState
========================
*/
void idRenderBackendGL::API_SetDefaultState() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_State
========================
*/
void idRenderBackendGL::API_State( uint64 stateBits, uint64 changedBits ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_BeginView
========================
*/
void idRenderBackendGL::API_BeginView() {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_Cull
========================
*/
void idRenderBackendGL::API_Cull( int cullType, int previousCullType ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_PolygonOffset
========================
*/
void idRenderBackendGL::API_PolygonOffset( float scale, float bias ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_DepthBoundsTest
========================
*/
void idRenderBackendGL::API_DepthBoundsTest( float zmin, float zmax ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_Viewport
========================
*/
void idRenderBackendGL::API_Viewport( const renderRect_t & rect ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_Scissor
========================
*/
void idRenderBackendGL::API_Scissor( const renderRect_t & rect ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_BindTexture
========================
*/
void idRenderBackendGL::API_BindTexture( int slot, textureKind_t kind, textureHandle_t texture ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_BindProgram
========================
*/
void idRenderBackendGL::API_BindProgram( programHandle_t program ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_Clear
========================
*/
void idRenderBackendGL::API_Clear( const clearDesc_t & clear ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_DestroyTexture
========================
*/
void idRenderBackendGL::API_DestroyTexture( textureHandle_t texture ) {
	NotMigrated( __func__ );
}

/*
========================
idRenderBackendGL::API_DrawIndexed
========================
*/
void idRenderBackendGL::API_DrawIndexed( const drawCall_t & dc ) {
	NotMigrated( __func__ );
}
