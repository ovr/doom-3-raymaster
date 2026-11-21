// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The resolve of the HDR scene (R13) into the output, on the render graph:

	  SceneColor (RGBA16F, radiance x preExposure) ──[MSAA: Karis resolve, compute]──┬──────────────────────────┐
	    ┌─[Exposure histogram, compute: 64 bins of EV100, a value per 2 x 2]──────────┘                          │
	    └─[Eye adaptation, one thread]─→ EyeAdaptation ─[copy]─→ readback: the next frame's preExposure          │
	  Combine LUT (32^3 RGBA16F, rebuilt when its parms change, compute) ──────────────────────────────────────┤
	  EyeAdaptation (the exposure) ────────────────────────────────────────────────────────────────────────────┤
	                                                                                  [Tonemap, raster]─→ output (RGBA8, the viewport)

	The LUT is indexed by a log coordinate of the exposed colour c: t = log2( c / K + 1 ) / L, with L chosen so
	that c = 1 lies exactly on texel 17 of 32 and t = 1 is c ≈ 31; a texel is sampled at t * 31/32 + 0.5/32.
	The tonemap writes encoded sRGB through the output's UNORM view, so SRGB_OUTPUT does not change it.

	The histogram and EyeAdaptation outlive the frame: the adaptation empties the histogram for the next one, and
	EyeAdaptation keeps the adapted EV100 (include/exposure.hlsli). The CPU reads its copy once the frame's fence
	was waited for (Present waits for it), so preExposure is the exposure of the last read-back resolve.

	The gamma composite: Encode turns a view's rectangle of the scene into its sRGB encoding after the lights
	and back after the fog, each through a copy and a raster pass (per sample under MSAA).
*/

#include "../PostProcessSettings.h"
#include "d3d12_RenderGraph.h"

class idD3D12InternalPasses;
class idD3D12SamplerHeap;
class idD3D12SceneTarget;

namespace d3d12Lut {
constexpr uint32					SIZE = 32;
constexpr float						K = 0.016f;
[[nodiscard]] float					Range();					// L = log2( 1 / K + 1 ) * 31 / 17
[[nodiscard]] float					Coordinate( float c );		// t of c, 0..1
[[nodiscard]] float					Value( float t );			// c of t
}

namespace d3d12Exposure {
constexpr uint32					BINS = 64;					// include/exposure.hlsli
constexpr uint32					HISTOGRAM_OFFSET = 16;		// of the counts in EyeAdaptation
constexpr uint32					EYE_BYTES = HISTOGRAM_OFFSET + BINS * 4;
constexpr uint32					WEIGHT_ONE = 32;			// a pixel's full weight
constexpr float					TARGET_STEPS = 16.0f;		// per EV: the target moves in steps of 1/16 EV
}

// The read-back EyeAdaptation of a resolve.
struct d3d12ExposureReadback_t {
	hdrExposure_t					exposure;
	std::array<uint32, d3d12Exposure::BINS>	histogram = {};
};

// b0 of internal/combineLut.hlsl.
struct d3d12LutParms_t {
	float							whiteBalance[3][4] = {};	// rows: out = M * c
	float							grade[15][4] = {};			// shadows, midtones, highlights x saturation .. offset
	float							options[4] = {};			// shadowsMax, highlightsMin, tonemapper, AgX look
	float							encoding[4] = {};			// K, L, white balance on, grade on
	constexpr bool					operator==( const d3d12LutParms_t & ) const = default;
};
[[nodiscard]] d3d12LutParms_t		D3D12_LutParms( const postProcessSettings_t & settings );

// The white of temp kelvin and tint (CIE daylight from 4000 K, Planckian below; the tint across the Planckian
// isotherm) adapted by Bradford to the white of 6500 K and tint 0, in linear sRGB: out = M * c.
[[nodiscard]] std::array<std::array<float, 3>, 3>	D3D12_WhiteBalance( float temp, float tint );

class idD3D12HdrPipeline {
public:
									idD3D12HdrPipeline( const idD3D12Device & device, idD3D12Submitter & submitter, const idD3D12ComputeRootSignature & computeRoot,
										const idD3D12InternalPasses & passes, idD3D12SamplerHeap & samplers, idD3D12DescriptorPool & srvPool );
									idD3D12HdrPipeline( const idD3D12HdrPipeline & ) = delete;
	idD3D12HdrPipeline &			operator=( const idD3D12HdrPipeline & ) = delete;

	// After the internal passes: the compute pipelines, the LUT and its sampler, the eye adaptation's buffers.
	void							CreateDefaults();
	// The HDR scene inside viewport, drawn with preExposure, into the output, which must both exist; the eye
	// adaptation moves by step. Executes the graph.
	void							Resolve( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, const postProcessSettings_t & settings,
										const renderRect_t & viewport, float preExposure, const hdrExposureStep_t & step, idD3D12DebugGroups * groups );
	// The next resolve's adaptation jumps to its target (a level load, a new output size).
	void							ResetExposure() { exposureReset = true; }
	// Reads the last resolve's EyeAdaptation once the submitter has waited for its fence.
	void							Poll();
	[[nodiscard]] const std::optional<d3d12ExposureReadback_t> &	LastExposure() const { return lastExposure; }
	// The HDR scene inside viewport to its sRGB encoding (toGamma) or back; executes the graph.
	void							Encode( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, bool toGamma,
										const renderRect_t & viewport, idD3D12DebugGroups * groups );
	[[nodiscard]] int				LutBuilds() const { return lutBuilds; }

private:
	// on first use per sample count
	[[nodiscard]] ID3D12PipelineState *	Tonemap( UINT samples );
	[[nodiscard]] ID3D12PipelineState *	EncodePipeline( UINT samples );

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	const idD3D12ComputeRootSignature &	computeRoot;
	const idD3D12InternalPasses &	passes;
	idD3D12SamplerHeap &			samplers;
	idD3D12DescriptorPool &			srvPool;
	d3d12ComputePipeline_t			combineLut;
	d3d12ComputePipeline_t			msaaResolve;
	d3d12ComputePipeline_t			exposureHistogram;
	d3d12ComputePipeline_t			exposureAdapt;
	std::array<idComPtr<ID3D12PipelineState>, 4>	tonemap;	// by log2 of the sample count
	std::array<idComPtr<ID3D12PipelineState>, 4>	encode;
	idComPtr<ID3D12Resource>		lut;
	d3d12ResourceState_t			lutState;
	d3d12Descriptor_t				lutSrv;
	d3d12Descriptor_t				lutUav;
	uint32							lutSampler = 0;		// linear, clamp: held for the context's life
	std::optional<d3d12LutParms_t>	lutParms;			// what the LUT holds
	int								lutBuilds = 0;
	idComPtr<ID3D12Resource>		histogram;			// BINS counts, empty between resolves
	d3d12ResourceState_t			histogramState;
	d3d12Descriptor_t				histogramUav;
	idComPtr<ID3D12Resource>		eyeAdaptation;		// include/exposure.hlsli
	d3d12ResourceState_t			eyeState;
	d3d12Descriptor_t				eyeSrv;
	d3d12Descriptor_t				eyeUav;
	idComPtr<ID3D12Resource>		eyeReadback;		// READBACK, EYE_BYTES
	uint64							readbackFence = 0;	// of the last copy into it; 0: nothing to read
	float							readbackPreExposure = 1.0f;
	std::optional<d3d12ExposureReadback_t>	lastExposure;
	bool							exposureReset = true;
};
