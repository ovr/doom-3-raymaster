// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	idRenderBackendD3D12: the Direct3D 12 implementation of idRenderBackend. Private to
	renderer/D3D12/: shared code reaches it only through renderBackend (R_CreateRenderBackend).
	Each override forwards to one subsystem of idD3D12Context; a draw reads the whole latch.
*/

#include "d3d12_Context.h"

extern idCVar					r_d3d12CheckImages;

// The console commands of d3d12_Diagnostics.cpp act on this context; nullptr outside Init .. Shutdown.
void							D3D12_SetActiveContext( idD3D12Context * context );

// r_d3d12ShowImage: looks the texture up by name when the name changes or the texture dies, draws it each frame.
class idD3D12ImageViewer {
public:
	void							Frame( idD3D12Context & context );	// before the context's EndFrame
	void							Reset();

private:
	idStr							name;
	textureHandle_t					texture = textureHandle_t::NONE;
	bool							reported = false;	// "no texture named" once per name
};

// r_d3d12Stats: sums the frames of the context and prints them once a second; the com_showFPS line.
class idD3D12StatsReport {
public:
	void							Frame( const idD3D12Context & context );	// after each Present
	[[nodiscard]] const char *		OverlayLine() const { return overlay; }		// "" while r_d3d12Stats is 0
	void							Reset();

private:
	d3d12FrameStats_t				sum;
	uint64							maxUploadBytes = 0;
	uint64							maxGpuMicroseconds = 0;
	int								frames = 0;
	int								startMsec = 0;
	char							overlay[96] = {};
};

class idRenderBackendD3D12 final : public idRenderBackend {
public:
									idRenderBackendD3D12();
									~idRenderBackendD3D12() override { Shutdown(); }

	std::expected<void, idStr>		Init( const renderBackendInit_t & init ) override;
	void							Shutdown() override;				// idempotent, safe after device loss and a mid-frame error
	void							ResizeOutput( int width, int height ) override;
	void							SetOption( renderOption_t option, bool enable ) override;
	void							SetGammaRamp( const unsigned short red[256], const unsigned short green[256], const unsigned short blue[256] ) override;
	void							CheckErrors() override;				// drains the info queues; reports a loss once
	void							PrintInfo() const override;
	const char *					OverlayStats() const override { return report.OverlayLine(); }

	void							BeginFrame() override;
	void							EndFrame() override;
	presentResult_t					Present( presentMode_t mode ) override;
	uint64							GetFrameGpuMicroseconds() override;
	void							ReadOutputPixels( int x, int y, int width, int height, byte * rgb ) override;

	textureHandle_t					CreateTexture( const idImageOpts & opts, textureRole_t role, const samplerDesc_t & sampler, const char * debugName ) override;
	void							UploadTexture( textureHandle_t texture, int mipLevel, int face, int width, int height, const void * pixels, int pixelPitch ) override;
	void							SetTextureSampler( textureHandle_t texture, const samplerDesc_t & sampler ) override;
	void							CopyOutputToTexture( textureHandle_t texture, int x, int y, int width, int height ) override;
	void							CopyDepthToTexture( textureHandle_t texture, int x, int y, int width, int height ) override;
	void							BeginHdrScene( const postProcessSettings_t & settings ) override;
	void							ResolveHdrScene( const postProcessSettings_t & settings, const renderRect_t & viewport,
										const hdrExposureStep_t & step ) override;
	void							EndHdrScene() override;
	void							SetHdrSceneGamma( bool encoded, const renderRect_t & viewport ) override;
	[[nodiscard]] std::optional<hdrExposure_t>	LastHdrExposure() const override;
	void							EndLevelLoad() override;			// r_d3d12CheckImages 1: d3d12_checkImages

	bufferHandle_t					CreateBuffer( bufferType_t type, bufferUsage_t usage, int size, const void * data ) override;
	void							DestroyBuffer( bufferHandle_t buffer ) override;
	void *							MapBuffer( bufferHandle_t buffer ) override;
	void							UnmapBuffer( bufferHandle_t buffer, int dirtyOffset, int dirtyBytes ) override;

	shaderHandle_t					CreateShaderStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info ) override;
	programHandle_t					CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * debugName, programInfo_t & info ) override;
	void							DestroyShaderStage( shaderHandle_t shader ) override;
	void							DestroyProgram( programHandle_t program ) override;

	void							WriteTimestamp( int set, int index ) override;
	bool							TimestampReady( int set, int index ) override;
	void							ReadTimestamps( int set, int count, uint64 * nanoseconds ) override;
	void							FreeTimestamps() override;
	void							PushDebugGroup( const char * name ) override;
	void							PopDebugGroup() override;

protected:
	void							API_Clear( const clearDesc_t & clear ) override;
	void							API_DestroyTexture( textureHandle_t texture ) override;
	void							API_DrawIndexed( const drawCall_t & dc ) override;

private:
	std::unique_ptr<idD3D12Context>	context;
	idD3D12StatsReport				report;
	idD3D12ImageViewer				viewer;
	d3d12GammaRamp_t				gamma = D3D12_IdentityGamma();	// the last SetGammaRamp
	bool							srgbOutput = false;				// the last SetOption( SRGB_OUTPUT )
};
