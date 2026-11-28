// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	r_rtReference's means of the ray traced signals (R18), per eye, RGBA32F (a half float stops counting at 2048):

	  shadow           the viewport's: rt shadow ratio's, what the shadows leave of the lights, a the unshadowed
	                   luminance
	  indirect amount  the indirect light's grid (R19), a texel per R_IndirectStride pixels:
	                   include/rtBounce.hlsli's luminance and where it comes from
	  indirect aux     its chroma, how far the ray went, 1
	  reflection       the viewport's: what the rays met (R20), a how far they went

	Each is a pair: a frame reads the surface the frame before wrote and writes the other. A surface remembers
	the epoch, the sequence it was written with (rtView_t) and the frames in its mean, and is read only at the next
	sequence of that epoch, so a pass that skipped a frame starts again; the same sequence again (a second graph of
	the frame) gets the same two. Views: SRV and UAV; the passes are compute. Drop buries everything: Resize,
	EndLevelLoad, Shutdown. Nothing is made without r_rtReference: NRD keeps the histories of the filtered frames.
*/

#include <array>

#include "../RayTracingSettings.h"
#include "d3d12_Descriptors.h"
#include "d3d12_Memory.h"

enum class rtHistorySlot_t : uint8 {
	SHADOW, INDIRECT_AMOUNT, INDIRECT_AUX, REFLECTION, COUNT
};

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
		uint32						frames = 0;			// in previous's mean
	};

									idD3D12RtHistory( const idD3D12Device & device, idD3D12Graveyard & graveyard, idD3D12DescriptorPool & srvPool );
									idD3D12RtHistory( const idD3D12RtHistory & ) = delete;
	idD3D12RtHistory &				operator=( const idD3D12RtHistory & ) = delete;

	// The slot's pair of view's eye, made when it is missing or of another size or format (then nothing to read).
	[[nodiscard]] use_t				Use( rtHistorySlot_t slot, DXGI_FORMAT format, uint32 width, uint32 height, const rtView_t & view );
	// The next of the slot's last Use holds view's frame now, a mean of frames.
	void							Written( rtHistorySlot_t slot, const rtView_t & view, uint32 frames );
	void							Drop();
	[[nodiscard]] uint64			Bytes() const { return bytes; }

private:
	struct pair_t {
		std::array<surface_t, 2>	surfaces;
		std::array<uint32, 2>		epoch = {};
		std::array<uint32, 2>		sequence = {};
		std::array<uint32, 2>		frames = {};
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
