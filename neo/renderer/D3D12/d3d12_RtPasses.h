// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The passes that trace the GPU scene (R15), on the render graph, over what idD3D12RayTracing built last:

	  rt reflect       COMPUTE  r_rtReflections (R20): a ray per pixel of the main view (internal/rtReflect.hlsl), from
	                            the depth and the G-buffer, a mirror's about the normal, a rougher pixel's of its lobe,
	                            its hit lit by the scene's lights → RGBA16F of the viewport, before the Fresnel
	  rt reflect temporal  COMPUTE  the glossy pixels' rays into their means over the frames, found where the surface
	                            and where the reflected image was (internal/rtReflectTemporal.hlsl), after rt guides
	  rt reflect atrous n  COMPUTE  r_rtReflectionPasses times over the neighbours of the same surface, as far as
	                            the lobe reaches on the screen (internal/rtReflectAtrous.hlsl)
	  rt composite     RASTER   that times the surface's Fresnel, the scale and preExposure, added to the HDR scene
	                            (internal/rtComposite.hlsl)
	  rt debug         RASTER   r_rtDebug: a ray per pixel of the main view (internal/rtDebug.hlsl) over the output
	  rt shadow ratio  RASTER   r_rtShadows, after the traced lights (R16): per pixel and channel what the shadows
	                            leave, 1 - the light target over the scene (internal/rtShadowRatio.hlsl) → RGBA16F
	  rt guides        COMPUTE  R18, for a filter: the frame's packed normal and depth, kept for the next frame, the
	                            way back to that frame's and the depth's slope (internal/rtGuides.hlsl)
	  rt shadow gradient  COMPUTE  per tile of 8 x 8 pixels, how far the ratio lies from what its history holds and the
	                            noise that explains it (internal/rtGradient.hlsl); only with a frame before
	  rt shadow temporal  COMPUTE  the ratio into its average over the frames, with the variance of its luminance
	                            (internal/rtTemporal.hlsl): idD3D12RtHistory holds the average and its moments
	  rt shadow atrous n  COMPUTE  r_rtShadowPasses times over the neighbours of the same surface, each pass twice
	                            as wide, as far as the variance says they differ by noise (internal/rtAtrous.hlsl)
	  rt shadow apply  RASTER   the scene times what is left (internal/rtShadowApply.hlsl)
	  rt indirect      COMPUTE  r_rtIndirect (R19): a ray of one bounce per texel of a grid of the main view, a texel
	                            per stride pixels (internal/rtIndirect.hlsl), after rt guides of that grid, whose
	                            normals are the depth's planes → what came and from where (include/rtBounce.hlsli)
	  rt indirect temporal  COMPUTE  that into its means over the frames (internal/rtIndirectTemporal.hlsl)
	  rt indirect atrous n  COMPUTE  r_rtIndirectPasses times over the neighbours of the same surface
	                            (internal/rtIndirectAtrous.hlsl)
	  rt indirect upsample  RASTER  every pixel takes it by its own normal, times its albedo, added to the HDR
	                            scene (internal/rtUpsample.hlsl)

	The rays read the scene through ResourceDescriptorHeap (include/raytracing.hlsli): the TLAS, the records, the
	instance table, the static and the skinned vertices, the static indexes, the lights, the records' and lights'
	textures (made readable first) and two linear samplers, repeating and clamped, held for the context's life.

	A render program that traces (the program table's `traces`) reads the same through b4 in its pixel stage:
	DrawResources is what the draw recorder asks per such draw. Its noise is renderer/BlueNoise.h as a texture.
*/

#include <array>

#include "../RayTracingSettings.h"
#include "d3d12_GpuScene.h"
#include "d3d12_RayTracing.h"
#include "d3d12_RenderGraph.h"

class idD3D12InternalPasses;
class idD3D12Nrd;
class idD3D12RtHistory;
enum class rtHistorySlot_t : uint8;
class idD3D12SceneTarget;
class idD3D12SamplerHeap;
class idD3D12TextureStore;

class idD3D12RtPasses {
public:
									idD3D12RtPasses( const idD3D12Device & device, const idD3D12ComputeRootSignature & computeRoot,
										idD3D12InternalPasses & passes, idD3D12SamplerHeap & samplers, idD3D12BufferStore & buffers,
										idD3D12TextureStore & textures, idD3D12Barriers & barriers );
									idD3D12RtPasses( const idD3D12RtPasses & ) = delete;
	idD3D12RtPasses &				operator=( const idD3D12RtPasses & ) = delete;
	void							CreateDefaults();	// the samplers and the noise texture
	void							SetNrd( idD3D12Nrd * denoiser ) { nrd = denoiser; }	// nullptr: r_rtDenoiser 1 filters as 2
	void							DestroyDefaults();	// the noise texture, before the texture store's DestroyAll

	// b4 of a draw that traces: the heap indices of what the last build holds, their transitions queued for the
	// open list. All zero (nothing valid) while the last build cannot be traced.
	[[nodiscard]] d3d12TraceResources_t	DrawResources( idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene );

	// After the resolve: view's rays over viewport of the output, executed. Nothing while the last build is not
	// valid or the device is lost.
	void							Debug( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
										rtDebug_t mode, const rtView_t & view, const renderRect_t & viewport, idD3D12DebugGroups * groups );
	// In the HDR scene's linear phase, after the G-buffer: the reflections of viewport added to its colour, or put
	// in its place (settings.alone), the glossy ones over view's history, executed. While the last build cannot be
	// traced the glossy pixels take what the history holds; nothing without one or without a G-buffer.
	void							Reflections( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history, idD3D12RayTracing & rt,
										idD3D12GpuScene & gpuScene, const rtReflectionSettings_t & settings, const rtView_t & view,
										const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups );
	// In the HDR scene's linear phase, after the bracket of the traced lights: viewport of the scene loses what the
	// light target holds of it, executed (settings.show: what is left, in the scene's place). Nothing without one.
	// settings.Filtered() and a G-buffer of the frame: what is left goes through view's history and the spatial filter,
	// or through NRD (settings.denoiser), which takes the light target's penumbrae as well.
	void							Shadows( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history,
										const rtShadowSettings_t & settings, const rtView_t & view, const renderRect_t & viewport,
										float preExposure, idD3D12DebugGroups * groups );
	// In the HDR scene's linear phase, after the lights and the G-buffer: one bounce of light added to viewport of
	// the scene (settings.alone: in its place, without the albedo), over view's history, executed. While the last
	// build cannot be traced the frame takes what the history holds; nothing without one or without a G-buffer.
	void							Indirect( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history, idD3D12RayTracing & rt,
										idD3D12GpuScene & gpuScene, const rtIndirectSettings_t & settings, const rtView_t & view,
										const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups );

private:
	// What a filter reads of its frame (include/rtDenoise.hlsli), in one graph.
	struct guideUses_t {
		d3d12GraphResource_t		guides;
		d3d12GraphResource_t		previous;	// the frame before's guides; not valid: nothing of it may be read
		d3d12GraphResource_t		reproject;
		d3d12GraphResource_t		slope;
	};
	// A filter's grid over the scene: a texel per stride pixels a side, kept in slot of the history, its normals the
	// G-buffer's bumped ones or the depth's planes.
	struct guideGrid_t {
		uint32						stride;
		rtHistorySlot_t				slot;
		bool						planes;
	};
	// Adds rt guides over rect of the scene. history: where the guides stay for the next frame (view.store) and
	// the frame before's come from; nullptr for a filter without one. nullopt without a G-buffer.
	[[nodiscard]] std::optional<guideUses_t>	AddGuides( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory * history,
										const rtView_t & view, const renderRect_t & viewport, const renderRect_t & rect, const guideGrid_t & grid );
	// Adds the filter of ratio (R18) and returns its result, ratio itself when it cannot run; length: the texture
	// whose alpha is the frames of the history, when there is one.
	[[nodiscard]] d3d12GraphResource_t	FilterShadows( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history,
										const rtShadowSettings_t & settings, const rtView_t & view, const renderRect_t & viewport,
										const renderRect_t & rect, d3d12GraphResource_t ratio, float preExposure, d3d12GraphResource_t & length );
	[[nodiscard]] ID3D12PipelineState *	DebugPipeline( UINT samples );
	[[nodiscard]] ID3D12PipelineState *	CompositePipeline( UINT samples, bool add );
	[[nodiscard]] ID3D12PipelineState *	ShadowRatioPipeline( bool multisampled );
	[[nodiscard]] ID3D12PipelineState *	ShadowApplyPipeline( UINT samples, bool show );
	[[nodiscard]] ID3D12PipelineState *	UpsamplePipeline( UINT samples, bool add );
	[[nodiscard]] const d3d12ComputePipeline_t *	ComputePipeline( d3d12ComputePipeline_t & pipeline, const char * stage, const wchar_t * name );
	// Whether the last build can be traced; makes the static buffers and the textures readable when it can.
	[[nodiscard]] bool				Traceable( idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene );

	const idD3D12Device &			device;
	const idD3D12ComputeRootSignature &	computeRoot;
	idD3D12InternalPasses &			passes;
	idD3D12SamplerHeap &			samplers;
	idD3D12BufferStore &			buffers;
	idD3D12TextureStore &			textures;
	idD3D12Barriers &				barriers;
	textureHandle_t					noise = textureHandle_t::NONE;
	// scene id, sequence and the store's state changes at the last MakeTexturesReadable: a draw asks per draw
	std::optional<std::array<uint64, 3>>	readable;
	std::array<idComPtr<ID3D12PipelineState>, 4>	debug;		// by log2 of the scene's samples, on first use
	std::array<idComPtr<ID3D12PipelineState>, 8>	composite;	// by add, then log2 of the samples
	std::array<d3d12ComputePipeline_t, 2>	reflect;		// by multisampled
	std::array<d3d12ComputePipeline_t, 2>	reflectTemporal;	// by multisampled
	d3d12ComputePipeline_t			reflectAtrous;
	std::array<idComPtr<ID3D12PipelineState>, 2>	shadowRatio;	// by multisampled
	std::array<idComPtr<ID3D12PipelineState>, 8>	shadowApply;	// by show, then log2 of the samples
	std::array<d3d12ComputePipeline_t, 2>	guides;			// by multisampled
	d3d12ComputePipeline_t			gradients;
	d3d12ComputePipeline_t			temporal;
	d3d12ComputePipeline_t			atrous;
	std::array<d3d12ComputePipeline_t, 2>	indirect;		// by multisampled
	d3d12ComputePipeline_t			indirectTemporal;
	d3d12ComputePipeline_t			indirectAtrous;
	std::array<idComPtr<ID3D12PipelineState>, 8>	upsample;	// by add, then log2 of the samples
	uint32							linearSampler = 0;
	uint32							clampSampler = 0;
	idD3D12Nrd *					nrd = nullptr;
};
