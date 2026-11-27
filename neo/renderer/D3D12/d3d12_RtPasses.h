// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The passes that trace the GPU scene (R15), on the render graph, over what idD3D12RayTracing built last:

	  rt reflect       COMPUTE  r_rtReflections (R20): a ray per pixel of the main view (internal/rtReflect.hlsl), from
	                            the depth and the G-buffer, a mirror's about the normal, a rougher pixel's of its lobe,
	                            its hit lit by the scene's lights → RGBA16F of the viewport, before the Fresnel;
	                            the glossy pixels' then go through NRD (idD3D12Nrd) or into r_rtReference's mean
	  rt composite     RASTER   that times the surface's Fresnel, the scale and preExposure, added to the HDR scene
	                            (internal/rtComposite.hlsl)
	  rt debug         RASTER   r_rtDebug: a ray per pixel of the main view (internal/rtDebug.hlsl) over the output
	  rt shadow ratio  RASTER   r_rtShadows, after the traced lights (R16): per pixel and channel what the shadows
	                            leave, 1 - the light target over the scene (internal/rtShadowRatio.hlsl) → RGBA16F;
	                            then through NRD (R18) or into r_rtReference's mean
	  rt shadow apply  RASTER   the scene times what is left (internal/rtShadowApply.hlsl)
	  rt guides        COMPUTE  R19: the indirect light's grid of the main view, a texel per stride pixels, its depth's
	                            plane packed and the depth's slope (internal/rtGuides.hlsl, include/rtGuide.hlsli)
	  rt indirect      COMPUTE  r_rtIndirect (R19): a ray of one bounce per texel of that grid (internal/rtIndirect.hlsl)
	                            → what came and from where (include/rtBounce.hlsli); then through NRD or into
	                            r_rtReference's means
	  rt indirect upsample  RASTER  every pixel takes the texels of its surface by its own normal, times its albedo,
	                            added to the HDR scene (internal/rtUpsample.hlsl)
	  rt ... mean      COMPUTE  r_rtReference: a signal into its plain mean over the frames while the eye stands still
	                            (internal/rtAccumulate.hlsl), which idD3D12RtHistory keeps

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
	void							SetNrd( idD3D12Nrd * denoiser ) { nrd = denoiser; }	// nullptr: nothing is filtered
	void							DestroyDefaults();	// the noise texture, before the texture store's DestroyAll

	// b4 of a draw that traces: the heap indices of what the last build holds, their transitions queued for the
	// open list. All zero (nothing valid) while the last build cannot be traced.
	[[nodiscard]] d3d12TraceResources_t	DrawResources( idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene );

	// After the resolve: view's rays over viewport of the output, executed. Nothing while the last build is not
	// valid or the device is lost.
	void							Debug( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
										rtDebug_t mode, const rtView_t & view, const renderRect_t & viewport, idD3D12DebugGroups * groups );
	// In the HDR scene's linear phase, after the G-buffer: the reflections of viewport added to its colour, or put
	// in its place (settings.alone), the glossy ones through NRD or r_rtReference's mean in history, executed. While
	// the last build cannot be traced the glossy pixels take what those hold; nothing without them or a G-buffer.
	void							Reflections( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history, idD3D12RayTracing & rt,
										idD3D12GpuScene & gpuScene, const rtReflectionSettings_t & settings, const rtView_t & view,
										const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups );
	// In the HDR scene's linear phase, after the bracket of the traced lights: viewport of the scene loses what the
	// light target holds of it, executed (settings.show: what is left, in the scene's place). Nothing without one.
	// settings.Penumbrae() and a G-buffer of the frame: what is left goes through NRD, which takes the light target's
	// penumbrae as well; settings.reference: into its mean in history.
	void							Shadows( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history,
										const rtShadowSettings_t & settings, const rtView_t & view, const renderRect_t & viewport,
										float preExposure, idD3D12DebugGroups * groups );
	// In the HDR scene's linear phase, after the lights and the G-buffer: one bounce of light added to viewport of
	// the scene (settings.alone: in its place, without the albedo), through NRD or r_rtReference's means, executed.
	// While the last build cannot be traced the frame takes what those hold; nothing without them or a G-buffer.
	void							Indirect( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history, idD3D12RayTracing & rt,
										idD3D12GpuScene & gpuScene, const rtIndirectSettings_t & settings, const rtView_t & view,
										const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups );

private:
	// What the upsample reads of the indirect light's grid (include/rtGuide.hlsli), in one graph.
	struct guideUses_t {
		d3d12GraphResource_t		guides;
		d3d12GraphResource_t		slope;
	};
	// Adds rt guides over rect of the scene, a texel per stride pixels a side. nullopt without a G-buffer.
	[[nodiscard]] std::optional<guideUses_t>	AddGuides( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, const rtView_t & view,
										const renderRect_t & viewport, const renderRect_t & rect, uint32 stride );
	// r_rtReference: signal into its plain mean over the frames in slot of history (internal/rtAccumulate.hlsl).
	// signal invalid: a frame without rays, the mean stays; restart: it starts again. Invalid when there is neither
	// a mean nor a signal.
	[[nodiscard]] d3d12GraphResource_t	Accumulate( idD3D12RenderGraph & graph, idD3D12RtHistory & history, const rtView_t & view,
										rtHistorySlot_t slot, uint32 width, uint32 height, d3d12GraphResource_t signal, bool restart );
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
	std::array<idComPtr<ID3D12PipelineState>, 2>	shadowRatio;	// by multisampled
	std::array<idComPtr<ID3D12PipelineState>, 8>	shadowApply;	// by show, then log2 of the samples
	std::array<d3d12ComputePipeline_t, 2>	guides;			// by multisampled
	std::array<d3d12ComputePipeline_t, 2>	indirect;		// by multisampled
	d3d12ComputePipeline_t			accumulate;
	std::array<idComPtr<ID3D12PipelineState>, 8>	upsample;	// by add, then log2 of the samples
	uint32							linearSampler = 0;
	uint32							clampSampler = 0;
	idD3D12Nrd *					nrd = nullptr;
};
