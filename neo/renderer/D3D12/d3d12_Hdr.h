// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The resolve of the HDR scene (R13) into the output, on the render graph:

	  SceneColor (RGBA16F, radiance x preExposure) ──[MSAA: Karis resolve, compute]──┬──────────────────────────┐
	    ├─[Exposure histogram, compute: 64 bins of EV100, a value per 2 x 2]──────────┘                          │
	    │   └─[Eye adaptation, one thread]─→ EyeAdaptation ─[copy]─→ readback: the next frame's preExposure      │
	    ├─[Bloom downsample: 13 taps, Karis on the first]─→ chain (RGBA16F, mips 1/2 .. 1/64 of the viewport)    │
	    │   └─ per level, smallest first: [blur X]─→[blur Y, x tint, + the smaller level's sum]─→ Bloom ───────┤
	    └─[Local exposure grid: 64 x 64 tiles x 32 bins of log2 luminance]─→[blur]─→ grid, blurred luminance ──┤
	  Combine LUT (32^3 RGBA16F, rebuilt when its parms change, compute) ──────────────────────────────────────┤
	  EyeAdaptation (the exposure) ────────────────────────────────────────────────────────────────────────────┤
	                                                                                  [Tonemap, raster]─→ output (RGBA8, the viewport)

	The LUT is indexed by a log coordinate of the exposed colour c: t = log2( c / K + 1 ) / L, with L chosen so
	that c = 1 lies exactly on texel 17 of 32 and t = 1 is c ≈ 31; a texel is sampled at t * 31/32 + 0.5/32.
	The tonemap writes encoded sRGB through the output's UNORM view, so SRGB_OUTPUT does not change it.

	The histogram and EyeAdaptation outlive the frame: the adaptation empties the histogram for the next one, and
	EyeAdaptation keeps the adapted EV100 (include/exposure.hlsli). Each resolve copies it into a slot of a ring the
	CPU reads once its fence was waited for: preExposure is the exposure of the newest resolve read back, the frame
	before the last one's with two frames in flight (Present waits for that one); after a reset, until a resolve is
	read back, the manual exposure.

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
// The exposed colour's scale before the curve: under every curve the demos keep the mean brightness of
// the LDR frames (analyze.py --luma). Doom 3's pixels sit far below mid grey, where the curves' toes differ.
[[nodiscard]] float					CurveScale( hdrTonemapper_t tonemapper, uint8 look );
}

// The bloom (internal/bloomDownsample.hlsl, bloomBlur.hlsl, PostProcessSettings.h): level 0 is half the viewport,
// each next one half the last; the tonemap lerps the scene toward level 0's sum.
namespace d3d12Bloom {
constexpr uint32					MAX_LEVELS = 6;
constexpr DXGI_FORMAT				FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;	// R11G11B10 stores round toward zero: -2.4 % over three
constexpr uint32					MAX_RADIUS = 64;			// taps on each side, internal/bloomBlur.hlsl
// The levels a viewport of width x height gets: 0 when nothing shows the bloom.
[[nodiscard]] uint32				Levels( const postProcessSettings_t & settings, int width, int height );
// Level's Gaussian in its texels, 3 sigma = its size times sizeScale percent of viewportWidth: weights[0..radius],
// which with the mirrored taps sum to 1.
[[nodiscard]] std::vector<float>	Weights( const postProcessSettings_t & settings, uint32 level, int viewportWidth );
// The tints of the levels in use, scaled so that their lumas sum to 1; all 0 when they sum to nothing.
[[nodiscard]] std::array<idVec3, MAX_LEVELS>	Tints( const postProcessSettings_t & settings, uint32 levels );
}

// The local exposure (internal/localExposureGrid.hlsl, localExposureBlur.hlsl, PostProcessSettings.h): a bilateral
// grid of TILE x TILE pixel cells and BINS bins of log2 luminance over the histogram's range, and the tiles' totals.
namespace d3d12LocalExposure {
constexpr uint32					TILE = 64;
constexpr uint32					BINS = 32;
constexpr DXGI_FORMAT				FORMAT = DXGI_FORMAT_R32G32_FLOAT;	// ( sum of log2 luminance above the range's start, count )
// Whether a resolve needs it: off, or every contrast 1 without visualize 4, is the identity.
[[nodiscard]] bool					Active( const postProcessSettings_t & settings );
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
	float							curve[4] = {};				// CurveScale; yzw unused
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
	// The next resolve's adaptation jumps to its target (a level load, a new output size). The readbacks are
	// dropped: the gamma-phase blends depend on preExposure, so the frames until the next one are drawn with
	// the manual exposure, as after Init, and give the same target whatever ran before.
	void							ResetExposure() { exposureReset = true; lastExposure.reset(); readbacks = {}; }
	// Reads the EyeAdaptation of the newest resolve whose fence the submitter has waited for; an older one is never
	// read after it.
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
	// The bloom chain's passes from input's viewport rect; returns the resource whose mip 0 holds the sum.
	[[nodiscard]] d3d12GraphResource_t	AddBloom( idD3D12RenderGraph & graph, const postProcessSettings_t & settings, d3d12GraphResource_t input,
										uint32 inputWidth, uint32 inputHeight, const renderRect_t & rect, uint32 levels );
	// The local exposure's passes from input's viewport rect: the blurred grid and the blurred luminance.
	[[nodiscard]] std::pair<d3d12GraphResource_t, d3d12GraphResource_t>	AddLocalExposure( idD3D12RenderGraph & graph,
										const postProcessSettings_t & settings, d3d12GraphResource_t input, const renderRect_t & rect, float invPreExposure );

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
	d3d12ComputePipeline_t			bloomDownsample;
	d3d12ComputePipeline_t			bloomBlur;
	d3d12ComputePipeline_t			localExposureGrid;
	d3d12ComputePipeline_t			localExposureBlur;
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
	// copies in flight: a resolve per view, of the frame recorded and two the GPU draws
	static constexpr uint32			EYE_READBACKS = 6;
	struct eyeReadback_t {
		uint64						fence = 0;			// of the copy into the slot; 0: nothing to read
		uint64						order = 0;			// resolves before it
		float						preExposure = 1.0f;
	};
	idComPtr<ID3D12Resource>		eyeReadback;		// READBACK, EYE_READBACKS slots of EYE_BYTES
	std::array<eyeReadback_t, EYE_READBACKS>	readbacks;
	uint64							resolves = 0;
	std::optional<d3d12ExposureReadback_t>	lastExposure;
	bool							exposureReset = true;
};
