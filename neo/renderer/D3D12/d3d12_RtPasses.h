// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The passes that trace the GPU scene (R15), on the render graph, over what idD3D12RayTracing built last:

	  rt reflect     COMPUTE  r_rtReflections: a mirror ray per pixel of the main view (internal/rtReflect.hlsl), from
	                          the depth and the G-buffer, its hit lit by the scene's lights → RGBA16F of the viewport
	  rt composite   RASTER   that times the scale and preExposure, added to the HDR scene (internal/rtComposite.hlsl)
	  rt debug       RASTER   r_rtDebug: a ray per pixel of the main view (internal/rtDebug.hlsl) over the output

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
	void							DestroyDefaults();	// the noise texture, before the texture store's DestroyAll

	// b4 of a draw that traces: the heap indices of what the last build holds, their transitions queued for the
	// open list. All zero (nothing valid) while the last build cannot be traced.
	[[nodiscard]] d3d12TraceResources_t	DrawResources( idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene );

	// After the resolve: view's rays over viewport of the output, executed. Nothing while the last build is not
	// valid or the device is lost.
	void							Debug( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
										rtDebug_t mode, const rtView_t & view, const renderRect_t & viewport, idD3D12DebugGroups * groups );
	// In the HDR scene's linear phase, after the G-buffer: the mirror reflections of viewport added to its colour,
	// or put in its place (settings.alone), executed. Nothing while the last build or the G-buffer is missing.
	void							Reflections( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt,
										idD3D12GpuScene & gpuScene, const rtReflectionSettings_t & settings, const rtView_t & view,
										const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups );

private:
	[[nodiscard]] ID3D12PipelineState *	DebugPipeline( UINT samples );
	[[nodiscard]] ID3D12PipelineState *	CompositePipeline( UINT samples, bool add );
	[[nodiscard]] const d3d12ComputePipeline_t *	ReflectPipeline( bool multisampled );
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
	uint32							linearSampler = 0;
	uint32							clampSampler = 0;
};
