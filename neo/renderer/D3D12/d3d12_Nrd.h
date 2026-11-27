// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	NVIDIA NRD (third_party/NRD, USE_NRD) as the denoiser of the ray traced passes, r_rtDenoiser 1 (R18). NRD hands
	out compute dispatches of its own compiled DXIL; each becomes a COMPUTE pass of the render graph:

	  root signatures    per pipeline's (textures, storage textures): b0 space1 a root CBV, s0 and s1 space1 NRD's
	                     two samplers static, a table of t0.. and u0.. in space0. NRD's DXIL binds registers, ours
	                     does not: these are the only root signatures besides the backend's two.
	  descriptor tables  a range of the shader-visible heap per pipeline and texture set, copied once from the
	                     textures' views and kept while the textures live: no slot is written again.
	  denoiser           per signal and eye, an NRD instance with its pools, inputs and outputs at the signal's
	                     size (the viewport's; the indirect light's grid), committed and imported into the graphs:
	                     REBLUR_SPECULAR, REBLUR_DIFFUSE_SH, SIGMA_SHADOW_TRANSLUCENCY;
	                     RESTART for a new epoch, a skipped frame or r_rtHistory 0, CLEAR_AND_RESTART for new
	                     textures. A screenshot (rtView_t::store false) has instances of its own that start anew
	                     and go with the frame.
	  guides             per eye and set (the pixels; the grid), NRD's IN_VIEWZ, IN_NORMAL_ROUGHNESS and IN_MV
	                     (internal/nrdGuides.hlsl), written once per frame for the signals that share them.

	NRD's textures hold rows from the top, as the D3D clip space of the matrices it is given: GL's projection
	with its z made 0 .. 1, GL's model view (NRD takes it as the right-handed one it is). View z is the clip w,
	the time between frames the view's (rtView_t::frameMs): NRD's own timer would make no two runs alike.
	Lengths are world units, 39.37 to a metre: NRD's defaults in metres are scaled by it. A grid's texel is its
	block's first pixel and the grid may reach past the viewport: its projection and motion are mapped to that.
*/

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../RayTracingSettings.h"
#include "d3d12_Descriptors.h"
#include "d3d12_Pipelines.h"
#include "d3d12_RenderGraph.h"

class idD3D12InternalPasses;
class idD3D12SceneTarget;
class idD3D12DebugGroups;

class idD3D12Nrd {
public:
									idD3D12Nrd( const idD3D12Device & device, const idD3D12Library & library, idD3D12Graveyard & graveyard,
										idD3D12DescriptorPool & srvPool, idD3D12InternalPasses & passes, const idD3D12ComputeRootSignature & computeRoot );
									~idD3D12Nrd();
									idD3D12Nrd( const idD3D12Nrd & ) = delete;
	idD3D12Nrd &					operator=( const idD3D12Nrd & ) = delete;

	[[nodiscard]] static bool		Available();	// built with NRD

	// The glossy reflections of rect (the viewport, inside the scene) through REBLUR_SPECULAR of view's eye: rays is
	// rt reflect's texture of rect, invalid when the frame traces nothing (then NRD's last output stands). Returns
	// what rt composite reads; nullopt without a G-buffer, an output of the frame before, or a lost device.
	[[nodiscard]] std::optional<d3d12GraphResource_t>	Reflections( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene,
										const rtReflectionSettings_t & settings, const rtView_t & view, const renderRect_t & rect, d3d12GraphResource_t rays );
	struct indirectOutput_t {
		d3d12GraphResource_t		amount;
		d3d12GraphResource_t		aux;
	};
	// The indirect light's grid (a texel per stride pixels of rect) through REBLUR_DIFFUSE_SH: amount and chroma are
	// rt indirect's, invalid when the frame traces nothing. Returns what rt indirect upsample reads, in rt indirect's
	// layout; nullopt as Reflections.
	[[nodiscard]] std::optional<indirectOutput_t>	Indirect( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene,
										const rtIndirectSettings_t & settings, const rtView_t & view, const renderRect_t & rect, uint32 stride,
										d3d12GraphResource_t amount, d3d12GraphResource_t chroma );
	// The traced shadows of rect (the viewport, inside the scene) through SIGMA_SHADOW_TRANSLUCENCY: ratio is rt shadow
	// ratio's, lights the light target with the penumbrae in its alpha (R16), read unless view.held (then NRD's last
	// output stands). Returns what rt shadow apply reads; nullopt as Reflections.
	[[nodiscard]] std::optional<d3d12GraphResource_t>	Shadows( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene,
										const rtShadowSettings_t & settings, const rtView_t & view, const renderRect_t & rect,
										d3d12GraphResource_t ratio, d3d12GraphResource_t lights );
	// After the resolve: the validation view of the last signal run with settings.validation over viewport of the
	// output, once, executed. Nothing when there is none of that size.
	void							DrawValidation( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, const renderRect_t & viewport,
										idD3D12DebugGroups * groups );
	void							Drop();		// every instance and texture through the graveyard: Resize, EndLevelLoad, Shutdown
	[[nodiscard]] uint64			Bytes() const { return bytes; }

private:
	enum class signal_t : uint8 { REFLECTION, INDIRECT, SHADOW, COUNT };
	static constexpr int			EYES = 3;	// the left (or only) eye, the right one, a screenshot's
	static constexpr size_t			PIXELS = 0;	// guide sets: a texel per pixel, the bumped normal
	static constexpr size_t			GRID = 1;	// the indirect light's grid, the plane
	static constexpr size_t			GUIDE_SETS = 2;

	struct texture_t {
		idComPtr<ID3D12Resource>	resource;
		d3d12ResourceState_t		state;
		d3d12Descriptor_t			srv;
		d3d12Descriptor_t			uav;
		uint64						bytes = 0;
	};
	struct guides_t {
		std::array<texture_t, 3>	textures;	// IN_VIEWZ, IN_NORMAL_ROUGHNESS, IN_MV
		uint32						width = 0;
		uint32						height = 0;
		uint32						stride = 1;			// pixels a side per texel
		uint32						epoch = 0;
		uint32						sequence = 0;
		std::array<float, 3>		limits = { 0.0f, 0.0f, 0.0f };
		bool						written = false;	// for epoch, sequence and limits
	};
	// Where NRD's texel centres lie in the viewport: uv = scale uv' + offset, v from the top.
	struct texelMap_t {
		float						scale[2] = { 1.0f, 1.0f };
		float						offset[2] = { 0.0f, 0.0f };
	};
	struct table_t {
		d3d12Descriptor_t			first;
		uint32						count = 0;
	};
	struct denoiser_t;
	struct rootSignature_t {
		uint32						textures = 0;
		uint32						storage = 0;
		idComPtr<ID3D12RootSignature>	signature;
	};

	[[nodiscard]] denoiser_t *		Denoiser( signal_t signal, int eye, uint32 width, uint32 height, uint32 stride, bool validation );
	[[nodiscard]] guides_t *		Guides( int eye, size_t set, uint32 width, uint32 height, uint32 stride );
	// Imports guides into the graph and adds nrd guides over rect unless they hold view's frame with limits already.
	// limits: the roughness that reflects nothing, the fade below it, the mirror's (R20's glossy pixels are material
	// 1); nullopt for a signal that reads no material: whatever the guides were last written with.
	[[nodiscard]] bool				WriteGuides( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, guides_t & guides, size_t set,
										const rtView_t & view, const renderRect_t & rect, const std::optional<std::array<float, 3>> & limits,
										std::unordered_map<const texture_t *, d3d12GraphResource_t> & imported );
	// The denoiser's dispatches of the frame, a graph pass each, after SetCommonSettings for view.
	[[nodiscard]] bool				AddDispatches( idD3D12RenderGraph & graph, denoiser_t & denoiser, guides_t & guides, const rtView_t & view,
										bool restart, const texelMap_t & map, std::unordered_map<const texture_t *, d3d12GraphResource_t> & imported );
	// A screenshot's instances live until the next frame that is none: its graph has been executed then.
	void							BuryScreenshot();
	[[nodiscard]] bool				CreateTexture( texture_t & texture, DXGI_FORMAT format, uint32 width, uint32 height, const wchar_t * name );
	void							BuryTexture( texture_t & texture );
	void							BuryDenoiser( denoiser_t & denoiser );
	void							BuryGuides( guides_t & guides );
	[[nodiscard]] ID3D12RootSignature *	RootSignature( uint32 textures, uint32 storage );
	[[nodiscard]] ID3D12PipelineState *	Pipeline( const void * dxil, uint64 size, ID3D12RootSignature & root );
	[[nodiscard]] const d3d12ComputePipeline_t *	ComputePipeline( d3d12ComputePipeline_t & pipeline, const char * stage, const wchar_t * name );
	[[nodiscard]] d3d12GraphResource_t	Import( idD3D12RenderGraph & graph, const char * name, texture_t & texture, uint32 width, uint32 height,
										std::unordered_map<const texture_t *, d3d12GraphResource_t> & imported );

	const idD3D12Device &			device;
	const idD3D12Library &			library;
	idD3D12Graveyard &				graveyard;
	idD3D12DescriptorPool &			srvPool;
	idD3D12InternalPasses &			passes;
	const idD3D12ComputeRootSignature &	computeRoot;
	std::array<std::array<std::unique_ptr<denoiser_t>, static_cast<size_t>( signal_t::COUNT )>, EYES>	denoisers;
	std::array<std::array<guides_t, GUIDE_SETS>, EYES>	guides;
	std::vector<rootSignature_t>	rootSignatures;
	std::unordered_map<const void *, idComPtr<ID3D12PipelineState>>	pipelines;	// by NRD's DXIL
	std::array<d3d12ComputePipeline_t, 2>	guidesPipeline;		// by multisampled
	d3d12ComputePipeline_t			reflectPack;
	d3d12ComputePipeline_t			reflectUnpack;
	d3d12ComputePipeline_t			indirectPack;
	d3d12ComputePipeline_t			indirectUnpack;
	std::array<d3d12ComputePipeline_t, 2>	shadowPack;		// by multisampled
	d3d12ComputePipeline_t			shadowUnpack;
	std::array<idComPtr<ID3D12PipelineState>, 4>	validation;	// by log2 of the output's samples
	uint64							bytes = 0;
	int								validationEye = -1;		// whose signal left a validation view to draw
	signal_t						validationSignal = signal_t::REFLECTION;
};
