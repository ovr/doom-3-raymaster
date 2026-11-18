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

#pragma once

#include "d3d12_Output.h"
#include "d3d12_Pipelines.h"
#include "d3d12_ShaderCompiler.h"

// The backend's own draws, with the one root signature and a triangle over the viewport. Each takes its
// room first (and may submit), then records; the caller invalidates the recorder afterwards.
class idD3D12InternalPasses {
public:
									idD3D12InternalPasses( const idD3D12Device & device, const idD3D12RootSignature & root, idD3D12ShaderCompiler & compiler,
										idD3D12Submitter & submitter, idD3D12Barriers & barriers, idD3D12UploadRing & constants,
										idD3D12DescriptorRing & descriptors, idD3D12SamplerHeap & samplers, idD3D12DescriptorPool & srvPool );
	// The shaders and the blit PSO: device work, so after construction. The black views fill the unused table slots.
	void							CreateDefaults( d3d12Descriptor_t black2D, d3d12Descriptor_t blackCube );
	// Scene → back buffer (in RENDER_TARGET), the rows flipped. An exact Load and an identity gamma copy the bytes.
	void							PresentBlit( d3d12Descriptor_t sceneSrv, D3D12_CPU_DESCRIPTOR_HANDLE backBuffer, int width, int height );
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

private:
	struct room_t {
		d3d12UploadSpan_t			parms;			// 256 bytes for root CBV b1
		std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>	table;
		std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>	samplerTable;
	};
	[[nodiscard]] std::optional<room_t>	TryRoom( std::span<const d3d12Descriptor_t> textures, const d3d12SamplerTableKey_t * samplerKey );
	// Full rings: submit, wait for the oldest chunk and take again, before anything is recorded.
	[[nodiscard]] room_t			Room( std::span<const d3d12Descriptor_t> textures, const d3d12SamplerTableKey_t * samplerKey = nullptr );
	[[nodiscard]] idComPtr<ID3D12PipelineState>	CreatePipeline( const d3d12Bytecode_t & pixelShader, DXGI_FORMAT format, UINT samples,
										uint8 writeMask, const wchar_t * name ) const;
	void							Record( ID3D12GraphicsCommandList1 & list, D3D12_CPU_DESCRIPTOR_HANDLE target, ID3D12PipelineState & pso,
										const renderRect_t & viewport, const room_t & room );
	void							ShowPanel( ID3D12PipelineState & pso, std::span<const d3d12Descriptor_t> textures, D3D12_CPU_DESCRIPTOR_HANDLE target,
										const renderRect_t & panel, bool alpha, float level );

	const idD3D12Device &			device;
	const idD3D12RootSignature &	root;
	idD3D12ShaderCompiler &			compiler;
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12UploadRing &				constants;
	idD3D12DescriptorRing &			descriptors;
	idD3D12SamplerHeap &			samplers;
	idD3D12DescriptorPool &			srvPool;
	std::array<d3d12Descriptor_t, 2>	black;		// by textureKind_t: the table slots a pass does not sample
	d3d12SamplerTableKey_t			showSamplers;	// s0 trilinear, s1 point, both clamped
	d3d12Bytecode_t					triangleVs, blitPs, clearPs, show2DPs, showCubePs;
	std::array<d3d12Bytecode_t, 2>	depthCopyPs;	// a single-sample and a multisampled depth view
	idComPtr<ID3D12PipelineState>	blit;
	std::array<idComPtr<ID3D12PipelineState>, 2>	depthCopy;	// [multisampled], on first use
	std::array<idComPtr<ID3D12PipelineState>, d3d12TargetId_t::COUNT * 16>	maskedClear;	// [target id * 16 + mask], on first use
	std::array<idComPtr<ID3D12PipelineState>, d3d12TargetId_t::COUNT * 2>	showImage;		// [target id * 2 + kind], on first use
};
