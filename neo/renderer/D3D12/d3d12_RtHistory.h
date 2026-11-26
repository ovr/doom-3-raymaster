// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	What the ray traced passes keep from frame to frame (R18), per eye, at the viewport's size:

	  guides         R32_UINT  the G-buffer's normal and the depth, packed (include/rtDenoise.hlsli): what a
	                           history is tested against, and a filter's edges
	  shadow colour  RGBA16F   rgb what the shadows leave of the lights, averaged; a the frames in the average
	                           (RGBA32F for r_rtReference: a half float stops counting at 2048)
	  shadow aux     RG16F     r the average of that luminance squared, g the unshadowed luminance, unexposed

	Each is a pair: a frame reads the surface the frame before wrote and writes the other. A surface remembers
	the epoch and the sequence it was written with (rtView_t) and is read only at the next sequence of that
	epoch, so a pass that skipped a frame starts again; the same sequence again (a second graph of the frame)
	gets the same two. Views: SRV and UAV; the passes are compute. Drop buries everything: Resize, EndLevelLoad,
	Shutdown. 32 bytes a pixel and eye for the shadows: 118 MB at 2560 x 1440.
*/

#include <array>

#include "../RayTracingSettings.h"
#include "d3d12_Descriptors.h"
#include "d3d12_Memory.h"

enum class rtHistorySlot_t : uint8 { GUIDES, SHADOW_COLOR, SHADOW_AUX, COUNT };

class idD3D12RtHistory {
public:
	struct surface_t {
		idComPtr<ID3D12Resource>	resource;
		d3d12ResourceState_t		state;
		d3d12Descriptor_t			srv;
		d3d12Descriptor_t			uav;
	};
	struct use_t {
		surface_t *					previous = nullptr;	// the frame before's; nullptr: nothing to read
		surface_t *					next = nullptr;		// this frame's; nullptr: it could not be made (a lost device)
	};

									idD3D12RtHistory( const idD3D12Device & device, idD3D12Graveyard & graveyard, idD3D12DescriptorPool & srvPool );
									idD3D12RtHistory( const idD3D12RtHistory & ) = delete;
	idD3D12RtHistory &				operator=( const idD3D12RtHistory & ) = delete;

	// The slot's pair of view's eye, made when it is missing or of another size or format (then nothing to read).
	[[nodiscard]] use_t				Use( rtHistorySlot_t slot, DXGI_FORMAT format, uint32 width, uint32 height, const rtView_t & view );
	// The next of the slot's last Use holds view's frame now.
	void							Written( rtHistorySlot_t slot, const rtView_t & view );
	void							Drop();
	[[nodiscard]] uint64			Bytes() const { return bytes; }

private:
	struct pair_t {
		std::array<surface_t, 2>	surfaces;
		std::array<uint32, 2>		epoch = {};
		std::array<uint32, 2>		sequence = {};
		std::array<bool, 2>			held = {};
		DXGI_FORMAT					format = DXGI_FORMAT_UNKNOWN;
		uint32						width = 0;
		uint32						height = 0;
		int							next = 0;
	};
	void							Bury( pair_t & pair );
	[[nodiscard]] bool				Create( pair_t & pair, rtHistorySlot_t slot );

	const idD3D12Device &			device;
	idD3D12Graveyard &				graveyard;
	idD3D12DescriptorPool &			srvPool;
	std::array<std::array<pair_t, static_cast<size_t>( rtHistorySlot_t::COUNT )>, 2>	eyes;
	uint64							bytes = 0;
};
