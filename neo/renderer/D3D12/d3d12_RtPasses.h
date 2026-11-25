// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The passes that trace the GPU scene (R15), on the render graph, over what idD3D12RayTracing built last:

	  rt debug   RASTER   r_rtDebug: a ray per pixel of the main view (internal/rtDebug.hlsl) over the output

	The rays read the scene through ResourceDescriptorHeap (include/raytracing.hlsli): the TLAS, the records, the
	instance table, the static and the skinned vertices, the static indexes, the records' textures (made readable
	first) and one linear, repeating sampler held for the context's life.
*/

#include <array>

#include "../RayTracingSettings.h"
#include "d3d12_GpuScene.h"
#include "d3d12_RayTracing.h"
#include "d3d12_RenderGraph.h"

class idD3D12InternalPasses;
class idD3D12SceneTarget;
class idD3D12SamplerHeap;

class idD3D12RtPasses {
public:
									idD3D12RtPasses( idD3D12InternalPasses & passes, idD3D12SamplerHeap & samplers, idD3D12BufferStore & buffers );
									idD3D12RtPasses( const idD3D12RtPasses & ) = delete;
	idD3D12RtPasses &				operator=( const idD3D12RtPasses & ) = delete;
	void							CreateDefaults();

	// After the resolve: view's rays over viewport of the output, executed. Nothing while the last build is not
	// valid or the device is lost.
	void							Debug( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
										const rtDebugView_t & view, const renderRect_t & viewport, idD3D12DebugGroups * groups );

private:
	[[nodiscard]] ID3D12PipelineState *	DebugPipeline( UINT samples );

	idD3D12InternalPasses &			passes;
	idD3D12SamplerHeap &			samplers;
	idD3D12BufferStore &			buffers;
	std::array<idComPtr<ID3D12PipelineState>, 4>	debug;	// by log2 of the scene's samples, on first use
	uint32							linearSampler = 0;
};
