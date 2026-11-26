// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "../RayTracingSettings.h"
#include "d3d12_Output.h"
#include "d3d12_Pipelines.h"
#include "d3d12_HlslCompiler.h"

// The ramp of r_gamma and r_brightness as R_SetColorMappings builds it: a 16-bit output per 8-bit input.
struct d3d12GammaRamp_t {
	std::array<uint16, 256>			red = {};
	std::array<uint16, 256>			green = {};
	std::array<uint16, 256>			blue = {};
	bool							operator==( const d3d12GammaRamp_t & ) const = default;
};
[[nodiscard]] d3d12GammaRamp_t		D3D12_IdentityGamma();	// i * 257: every byte maps to itself
// True when the blit through the ramp gives every byte back, round( v * 255 / 65535 ) == i in each channel:
// the plain blit then copies the same bytes without the LUT.
[[nodiscard]] bool					D3D12_GammaIsIdentity( const d3d12GammaRamp_t & ramp );

// The backend's own draws, with the one root signature and a triangle over the viewport. Each takes its
// parms first (and may submit), then records; the caller invalidates the recorder afterwards.
// The backend's own stages, shaders/internal/<file>.hlsl: CreateDefaults builds them, d3d12_compileAll too.
struct d3d12InternalStage_t {
	const char *					file;
	shaderStageType_t				type;
	const char *					name;
	const char *					define;		// nullptr or "NAME=VALUE"
};
[[nodiscard]] std::span<const d3d12InternalStage_t>	D3D12_InternalStages();
// The stage's file with its define; nullopt when the file is missing.
[[nodiscard]] std::optional<d3d12HlslSource_t>		D3D12_InternalSource( const d3d12InternalStage_t & stage, const d3d12ShaderReader_t & reader );

// How the colour of an internal pipeline meets its target.
enum class d3d12PassBlend_t : uint8 {
	REPLACE,
	ADD,			// target + colour
	MULTIPLY		// target * colour
};

class idD3D12InternalPasses {
public:
									idD3D12InternalPasses( const idD3D12Device & device, const idD3D12RootSignature & root, idD3D12HlslCompiler & compiler,
										idD3D12Submitter & submitter, idD3D12Barriers & barriers, idD3D12Uploader & uploader, idD3D12UploadRing & constants,
										idD3D12SamplerHeap & samplers, idD3D12DescriptorPool & srvPool );
	// The shaders and the blit PSO: device work, so after construction.
	void							CreateDefaults();
	// Scene → back buffer (in RENDER_TARGET), the rows flipped. An exact Load and an identity gamma copy the bytes;
	// any other ramp maps each channel through the LUT, as the hardware ramp of id's OpenGL renderer mapped the desktop.
	void							PresentBlit( d3d12Descriptor_t sceneSrv, D3D12_CPU_DESCRIPTOR_HANDLE backBuffer, int width, int height );
	// Between calls of the contract: the first ramp that is no identity creates the 256 x 1 LUT and its PSO.
	void							SetGamma( const d3d12GammaRamp_t & ramp );
	[[nodiscard]] bool				GammaIsIdentity() const { return gammaIdentity; }
	// A colour clear with a partial write mask: the target (in RENDER_TARGET) inside rect, which lies inside it.
	void							MaskedClear( D3D12_CPU_DESCRIPTOR_HANDLE target, d3d12TargetId_t id, const renderRect_t & rect,
										const float rgba[4], uint8 mask );
	// r_d3d12ShowImage over the target (in RENDER_TARGET): a 2D image as RGB and alpha panels, a cube as a
	// cross of its faces (+Y above, -X +Z +X -Z, -Y below), each upright as GL samples it. The image is readable.
	void							ShowImage( d3d12Descriptor_t imageSrv, textureKind_t kind, int imageWidth, int imageHeight,
										D3D12_CPU_DESCRIPTOR_HANDLE target, d3d12TargetId_t id, int targetWidth, int targetHeight );
	// The depth box at (x, y) of the scene (its R24 view, sample 0 with MSAA, in DEPTH_READ | PIXEL_SHADER_RESOURCE)
	// into the whole width x height R32_FLOAT target (in RENDER_TARGET), rows in order. Texels outside the scene read 0.
	void							CopyDepth( d3d12Descriptor_t depthSrv, UINT samples, D3D12_CPU_DESCRIPTOR_HANDLE target, int x, int y,
										int width, int height );
	// r_gbufferDebug: a target of the G-buffer (its views, in PIXEL_SHADER_RESOURCE, sample 0) as colour over
	// viewport of the target (in RENDER_TARGET), which has the G-buffer's size.
	void							GBufferDebug( std::span<const d3d12Descriptor_t, idD3D12SceneTarget::GBUFFER_TARGETS> gbufferSrvs, UINT samples,
										gbufferDebug_t mode, D3D12_CPU_DESCRIPTOR_HANDLE target, d3d12TargetId_t id, const renderRect_t & viewport );
	// A stage of D3D12_InternalStages() by its name, after CreateDefaults; a fatal error for an unknown one.
	[[nodiscard]] const d3d12HlslStage_t &	Stage( const char * name ) const;
	// The triangle and pixelShader into one target of format, no depth: also the render graph's raster passes.
	[[nodiscard]] idComPtr<ID3D12PipelineState>	CreatePipeline( const d3d12Bytecode_t & pixelShader, DXGI_FORMAT format, UINT samples,
										uint8 writeMask, const wchar_t * name, d3d12PassBlend_t blend = d3d12PassBlend_t::REPLACE ) const;

private:
	// The 256 bytes of root CBV b1. A full ring: submit, wait for the oldest chunk and take again, before anything is recorded.
	[[nodiscard]] d3d12UploadSpan_t	Parms();
	void							Record( ID3D12GraphicsCommandList4 & list, D3D12_CPU_DESCRIPTOR_HANDLE target, ID3D12PipelineState & pso,
										const renderRect_t & viewport, const d3d12UploadSpan_t & parms, const d3d12DrawResources_t * resources );
	void							ShowPanel( ID3D12PipelineState & pso, const d3d12DrawResources_t & resources, D3D12_CPU_DESCRIPTOR_HANDLE target,
										const renderRect_t & panel, bool alpha, float level );
	[[nodiscard]] bool				CreateGammaLut();	// the texture, its view and the gamma blit PSO; false on a lost device

	const idD3D12Device &			device;
	const idD3D12RootSignature &	root;
	idD3D12HlslCompiler &			compiler;
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12Uploader &				uploader;
	idD3D12UploadRing &				constants;
	idD3D12SamplerHeap &			samplers;
	idD3D12DescriptorPool &			srvPool;
	std::array<uint32, 2>			showSamplers = {};	// the heap slots of sampler0 trilinear and sampler1 point, both clamped
	std::vector<d3d12HlslStage_t>	stages;			// every stage of INTERNAL_STAGES, in its order
	d3d12Bytecode_t					triangleVs, blitPs, blitGammaPs, clearPs, show2DPs, showCubePs;
	std::array<d3d12Bytecode_t, 2>	depthCopyPs;	// a single-sample and a multisampled depth view
	idComPtr<ID3D12PipelineState>	blit, blitGamma;
	idComPtr<ID3D12Resource>		gammaLut;		// 256 x 1 R16G16B16A16_UNORM, the ramp by input byte
	d3d12ResourceState_t			gammaLutState;
	d3d12Descriptor_t				gammaLutSrv;
	d3d12GammaRamp_t				gamma = D3D12_IdentityGamma();
	bool							gammaIdentity = true;
	std::array<idComPtr<ID3D12PipelineState>, 2>	depthCopy;	// [multisampled], on first use
	std::array<idComPtr<ID3D12PipelineState>, d3d12TargetId_t::COUNT * 16>	maskedClear;	// [target id * 16 + mask], on first use
	std::array<idComPtr<ID3D12PipelineState>, d3d12TargetId_t::COUNT * 2>	showImage;		// [target id * 2 + kind], on first use
	std::array<idComPtr<ID3D12PipelineState>, d3d12TargetId_t::COUNT>	gbufferDebug;	// [target id], on first use
};
