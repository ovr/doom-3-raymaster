/*
===========================================================================

Copyright (C) 2026 Dmitry Patsura

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Please do not use this file to train or fine-tune AI models.

===========================================================================
*/

#pragma once

#include "d3d12_Device.h"

#include <bitset>

/*
	GPU timestamps: one TIMESTAMP query heap, resolved into a READBACK buffer with the same slots.

	  slot 0, 1                              the frame timer: BeginFrame, EndFrame → GetFrameGpuMicroseconds
	  FIRST_SET + set * SLOTS_PER_SET + i    r_pix 1: WriteTimestamp( set, i ), TIMESTAMP_SETS sets

	EndFrame ends the frame timer and resolves it and every slot the frame wrote, in the open list, tagged
	with that list's fence as the graveyard tags its graves. A slot is ready once queue.Waited() has reached
	the tag: Present waits for its frame, so a frame's times are there after its Present. Polling reads no
	fence, waits for nothing and frees nothing, so r_pix cannot change what two runs record.
*/
class idD3D12GpuTimers {
public:
	static constexpr int			SLOTS_PER_SET = 2048;	// RenderLog: 1024 blocks of two timestamps

									idD3D12GpuTimers( const idD3D12Device & device, const idD3D12Queue & queue );
	[[nodiscard]] bool				Available() const { return heap != nullptr; }	// caps.gpuTimer, caps.timestamps
	[[nodiscard]] uint64			Frequency() const { return frequency; }
	void							BeginFrame( ID3D12GraphicsCommandList1 & list );
	void							Write( ID3D12GraphicsCommandList1 & list, int set, int index );
	void							EndFrame( ID3D12GraphicsCommandList1 & list );	// the end stamp and the resolves; submit next
	[[nodiscard]] bool				Ready( int set, int index ) const;
	void							Read( int set, std::span<uint64> nanoseconds ) const;	// zeros unless the set is ready
	// The frame EndFrame resolved last, once its list was waited for (Present does): never blocks; 0 before.
	[[nodiscard]] uint64			FrameMicroseconds();
	void							Free();					// every r_pix slot unwritten; the frame timer stays
	[[nodiscard]] int				WrittenThisFrame() const { return writes; }
	void							ResetFrameCounters() { writes = 0; }

private:
	static constexpr UINT			FRAME_BEGIN = 0;
	static constexpr UINT			FRAME_END = 1;
	static constexpr UINT			FIRST_SET = 2;
	static constexpr UINT			SLOTS = FIRST_SET + TIMESTAMP_SETS * SLOTS_PER_SET;

	struct set_t {
		std::bitset<SLOTS_PER_SET>	written;		// this frame, resolved by EndFrame
		std::bitset<SLOTS_PER_SET>	resolved;		// by the list of fence
		uint64						fence = 0;		// 0: nothing resolved
	};

	[[nodiscard]] static constexpr UINT	Slot( int set, int index ) { return FIRST_SET + static_cast<UINT>( set * SLOTS_PER_SET + index ); }
	void							Resolve( ID3D12GraphicsCommandList1 & list, UINT first, UINT count ) const;
	[[nodiscard]] bool				CopyTicks( UINT first, std::span<uint64> ticks ) const;

	const idD3D12Device &			device;
	const idD3D12Queue &			queue;
	idComPtr<ID3D12QueryHeap>		heap;
	idComPtr<ID3D12Resource>		readback;
	uint64							frequency = 0;			// ticks per second of the direct queue
	std::array<set_t, TIMESTAMP_SETS>	sets;
	bool							frameBegun = false;
	uint64							frameFence = 0;			// the list that resolved the last frame timer
	uint64							frameReadFence = 0;		// frameMicroseconds is the time of this list's frame
	uint64							frameMicroseconds = 0;
	int								writes = 0;
};

/*
	The debug groups of r_pix 2 for PIX and RenderDoc: BeginEvent / EndEvent with a UTF-16 name
	(metadata 0, PIX's unicode event). A group of shared code can outlive a list, so the stack lives
	here, not in the list: SubmitChunk ends every open group in the list it closes, and the next list
	begins them again in the same order. While a list is open, every group of the stack is open in it.

	  Push   → BeginEvent in the open list (opened on demand)
	  Pop    → EndEvent if a list is open
	  CloseAll (SubmitChunk), Reopen (a new list)
*/
class idD3D12DebugGroups {
public:
	static constexpr int			MAX_DEPTH = 64;		// caps.maxDebugGroupDepth
	static constexpr int			MAX_NAME = 64;		// characters with the terminator; longer names are cut

	void							Push( ID3D12GraphicsCommandList1 & list, const char * name );
	void							Pop( ID3D12GraphicsCommandList1 * list );	// nullptr: no list is open
	void							CloseAll( ID3D12GraphicsCommandList1 & list );
	void							Reopen( ID3D12GraphicsCommandList1 & list );
	[[nodiscard]] int				Depth() const { return depth; }
	[[nodiscard]] int				PushedThisFrame() const { return pushed; }
	void							ResetFrameCounters() { pushed = 0; }

private:
	void							Begin( ID3D12GraphicsCommandList1 & list, int level ) const;

	std::array<std::array<wchar_t, MAX_NAME>, MAX_DEPTH>	names = {};
	int								depth = 0;
	int								ignored = 0;		// pushes past MAX_DEPTH, so the pops stay balanced
	int								pushed = 0;
};
